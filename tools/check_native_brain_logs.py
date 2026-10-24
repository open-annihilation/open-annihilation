#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A brain log and a training record repeat, and the match does not change.

steady-commander draws a point within 600 world units of its commander's
start every 150 ticks, orders that move when the unit offers one, and every
300 ticks records an 8-by-8 grid of its own ground and what it has explored,
with the move's east-west offset as the decision. It plays Ashap Plateau at
seed 4242 for 1800 ticks against the built-in player.
"""

import argparse
import json
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

MANIFEST = """oabrain: 1
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

# The record sits inside the 150-tick move, on every 300th tick.
SCRIPT = """local spent = 0
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
    if tick % 300 == 0 then
      local grid = brain.grid({"own", "explored"}, 8)
      brain.record{features = {sectors = grid}, decision = dx}
    end
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


class ScriptFailure(Exception):
    """A play that did not do what the check asked."""


def plant(folder):
    """Writes the recording brain: a manifest and brain.lua."""
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "oabrain.yaml").write_text(MANIFEST, encoding="utf-8")
    (folder / "brain.lua").write_text(SCRIPT, encoding="utf-8")


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


def match_command(package, extra):
    """The shared headless command, plus the files this play writes."""
    return [
        "--headless-check",
        "--brain-match",
        f"{package},builtin",
        "--brain-map",
        MAP,
        "--seed",
        SEED,
        "--match-ticks",
        TICKS,
        *extra,
    ]


def steady_report(lines, output):
    """The played report: calm against hard."""
    require(lines, output, "the report is empty")
    require(HEADING.match(lines[0]) is not None, output, f"heading was {lines[0]!r}")
    require(STEADY.match(lines[1]) is not None, output, f"seat 1 was {lines[1]!r}")
    require(BUILTIN_HARD.match(lines[2]) is not None, output, f"seat 2 was {lines[2]!r}")
    require(DIGEST.search("\n".join(lines)) is not None, output, "the digest is missing")


def tick_seats(text):
    """Engine indexes named by tick lines."""
    found = set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[0] == "tick" and parts[2] == "seat":
            found.add(parts[3])
    return found


def record_ok(path, output):
    """The record file: a header, six grids, and two results."""
    lines = path.read_text(encoding="utf-8").splitlines()
    require(len(lines) == 9, output, f"the record has {len(lines)} lines")
    header = json.loads(lines[0])
    require(header["kind"] == "header" and header["format"] == 1, output, "the header kind is wrong")
    require(header["map"] == MAP and header["seed"] == int(SEED), output, "the header map or seed is wrong")
    seats = header["seats"]
    require(len(seats) == 2, output, "the header does not name two seats")
    require(
        seats[0]["seat"] == 1
        and seats[0]["brain"] == "steady-commander"
        and seats[0]["version"] == "1.2"
        and seats[0]["level"] == "calm",
        output,
        f"seat 1 was {seats[0]!r}",
    )
    require("version" not in seats[1], output, "the built-in seat names a version")
    require(
        seats[1]["seat"] == 2 and seats[1]["brain"] == "builtin" and seats[1]["level"] == "hard",
        output,
        f"seat 2 was {seats[1]!r}",
    )
    for index, line in enumerate(lines[1:7], start=1):
        sample = json.loads(line)
        require(sample["kind"] == "sample" and sample["seat"] == 1, output, f"sample {index} is not seat 1")
        feature = sample["features"]["sectors"]
        require(feature["shape"] == [2, 8, 8], output, f"sample {index} shape was {feature['shape']!r}")
        require(len(feature["values"]) == 128, output, f"sample {index} has {len(feature['values'])} values")
        require(isinstance(sample["decision"], int), output, f"sample {index} decision is not an integer")
    for index, line in enumerate(lines[7:9], start=1):
        result = json.loads(line)
        require(result["kind"] == "result" and result["seat"] == index, output, f"result {index} was {result!r}")
        require(isinstance(result["winner"], bool), output, f"result {index} winner is not a bool")
        require(isinstance(result["tick"], int) and isinstance(result["units"], int), output, "a result number is wrong")


def log_ok(path, output):
    """The brain log starts with its version and names both seats."""
    text = path.read_text(encoding="utf-8")
    require(text.startswith("brain log 1\n"), output, "the brain log does not start with its version")
    seats = tick_seats(text)
    require("0" in seats and "1" in seats, output, f"tick seats were {sorted(seats)}")


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
    with tempfile.TemporaryDirectory(prefix="T03-brain-logs-", dir=arguments.scratch_root) as scratch:
        work = Path(scratch)
        package = work / "packages" / "steady-commander"
        plant(package)
        user = work / "empty-user"
        user.mkdir()

        first_log = work / "first.log"
        first_record = work / "first.rec"
        first_trace = work / "first.trace"
        status, first_out = play(
            native,
            game_dir,
            work,
            user,
            "first",
            match_command(
                package,
                [
                    "--brain-trace",
                    str(first_log),
                    "--brain-record",
                    str(first_record),
                    "--trace-digest",
                    str(first_trace),
                ],
            ),
        )
        require(status == 0, first_out, f"the first play exited {status}")
        first_lines = report_lines(first_out)
        steady_report(first_lines, first_out)
        log_ok(first_log, first_out)
        record_ok(first_record, first_out)

        second_log = work / "second.log"
        second_record = work / "second.rec"
        second_trace = work / "second.trace"
        status, second_out = play(
            native,
            game_dir,
            work,
            user,
            "second",
            match_command(
                package,
                [
                    "--brain-trace",
                    str(second_log),
                    "--brain-record",
                    str(second_record),
                    "--trace-digest",
                    str(second_trace),
                ],
            ),
        )
        require(status == 0, second_out, f"the second play exited {status}")
        second_lines = report_lines(second_out)
        require(second_lines == first_lines, second_out, "the two reports differ")
        require(second_log.read_bytes() == first_log.read_bytes(), second_out, "the brain logs differ")
        require(second_record.read_bytes() == first_record.read_bytes(), second_out, "the records differ")
        require(second_trace.read_bytes() == first_trace.read_bytes(), second_out, "the trace streams differ")

        plain_trace = work / "plain.trace"
        status, plain_out = play(
            native,
            game_dir,
            work,
            user,
            "plain",
            match_command(package, ["--trace-digest", str(plain_trace)]),
        )
        require(status == 0, plain_out, f"the play without files exited {status}")
        require(report_lines(plain_out) == first_lines, plain_out, "the report changed when no file was written")
        require(plain_trace.read_bytes() == first_trace.read_bytes(), plain_out, "the trace stream changed")

        status, refused = play(
            native,
            game_dir,
            work,
            user,
            "needs-headless",
            ["--brain-trace", str(work / "unused.log"), "--match-ticks", TICKS],
        )
        require(status == 1, refused, f"a brain log without --headless-check exited {status}")

        missing = work / "missing" / "log.txt"
        status, refused = play(
            native,
            game_dir,
            work,
            user,
            "missing-folder",
            match_command(package, ["--brain-trace", str(missing)]),
        )
        require(status == 1, refused, f"a brain log in a missing folder exited {status}")
        require(not missing.exists(), refused, "the missing folder was created")

    print("brain log check: two plays agree, the match is unchanged, and refusals exit 1")


if __name__ == "__main__":
    try:
        main()
    except ScriptFailure as failure:
        print(f"brain log check: {failure}")
        raise SystemExit(1) from None
