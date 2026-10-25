#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Both machines of a loopback match give their brains the same starts.

The script writes Brains/start-reader into a scratch user folder, and a
scratch preferences file that makes it the default brain, so every computer
seat on both machines takes it. Start Reader prints `game.starts` and every
player's start from `me:players()` on its first turn, by name in name order,
then builds as Steady Commander does in native-net-loopback-brain and stops,
so the engine's computer player takes the strike the loopback check requires.

It runs one headless loopback of 600 ticks with fixed starts, in which the
host and the joiner each seat one computer player. Each machine seats the
players in slots of its own, so the joiner's slots do not follow the host's,
and the host deals the fixed starts in its own slot order. The host sends
each machine only its own players' starts, so each machine works out the
other's from the host's slot table. The script reads the two
seat logs and requires one line in each, both saying `fixed` and naming the
same start for every player: 1 to 4, each once. The script exits 77 when no
game data is installed.
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

RUN_TIMEOUT_SECONDS = 900
PREFERENCES_HEADER = "open-annihilation-preferences 1"
PLAYS = "plays start-reader 1.0"
BUILT = "net loopback check: the computer player built"
STARTS = re.compile(r"^tick \d+: starts (\w+): (.*)$", re.M)
ROW = re.compile(r"^(.+)=(\d+|none)$")

READER_MANIFEST = """oabrain: 1
id: start-reader
name: "Start Reader"
version: "1.0"
author:
  name: "Ridge Testers"
description: "Says where every player starts."
tags: ["teaching"]
works-with: ["*"]
levels:
  - id: firm
    name: "Firm"
    difficulty: hard
"""

# The building half is Steady Commander's from native-net-loopback-brain:
# four frames late in the build window, then a stop, so the engine's
# computer player builds on and takes the strike.
READER_SCRIPT = """local hooks = {}
local issued = 0
local watch = 0
local walking = false
local used = {}
local said = false

local function start_of(row)
  if row.start == nil then
    return "none"
  end
  return tostring(row.start)
end

-- Each machine numbers the players by its own slots, so the rows go by
-- name, in name order.
local function say_starts()
  local rows = me:players()
  local parts = {}
  for i = 1, #rows do
    parts[#parts + 1] = tostring(rows[i].name) .. "=" .. start_of(rows[i])
  end
  table.sort(parts)
  print("starts " .. tostring(game.starts) .. ": " .. table.concat(parts, ", "))
end

local function commander_of(units)
  for i = 1, #units do
    local kind = units[i].type
    if kind and kind.builder and kind.canmove and kind.candgun then
      return units[i]
    end
  end
  return nil
end

local function yards_of(commander)
  local list = game.build_list(commander.type)
  local yards = {}
  for i = 1, #list do
    local kind = list[i]
    if kind and not kind.canmove and not kind.builder and kind.buildtime then
      yards[#yards + 1] = kind
    end
  end
  table.sort(yards, function(a, b)
    return a.buildtime < b.buildtime
  end)
  return yards
end

function hooks.onTick(tick)
  if not said then
    said = true
    say_starts()
  end
  if tick >= 700 then
    error("the base is up")
  end
  if tick < 350 or (issued >= 4 and not walking) then
    return
  end
  local units = me:units()
  if walking then
    if #units > watch then
      walking = false
    else
      return
    end
  end
  if issued >= 4 then
    return
  end
  local commander = commander_of(units)
  if commander == nil then
    return
  end
  local yards = yards_of(commander)
  local spot = {x = commander.x, z = commander.z}
  for y = 1, #yards do
    local yard = yards[y]
    local cell_x, cell_z, x, z = brain.place(yard, spot, 200, 0)
    local key = cell_x ~= nil and (tostring(cell_x) .. ":" .. tostring(cell_z)) or nil
    if key ~= nil and not used[key] and brain.site(yard, cell_x, cell_z, 0, "yard")
        and commander:build(yard, {x = x, z = z}, 0, false) then
      used[key] = true
      issued = issued + 1
      watch = #units
      walking = true
      return
    end
  end
end

return hooks
"""


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_brain(user):
    """Writes Start Reader: a manifest and its script."""
    folder = user / "Brains" / "start-reader"
    folder.mkdir(parents=True)
    (folder / "oabrain.yaml").write_text(READER_MANIFEST, encoding="utf-8")
    (folder / "brain.lua").write_text(READER_SCRIPT, encoding="utf-8")


def write_preferences(path):
    """A preferences file whose default brain is Start Reader."""
    lines = [
        PREFERENCES_HEADER,
        quote_preference("open-annihilation.content-updates") + " " + quote_preference("never"),
        quote_preference("open-annihilation.first-run") + " " + quote_preference("done"),
        quote_preference("open-annihilation.brain.default") + " " + quote_preference("start-reader"),
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_game(native, game_dir, work, arguments):
    """Runs one headless game. Returns its status and combined output."""
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
        *arguments,
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


def start_lines(data):
    """Each seat log's starts lines, by log name."""
    lines = {}
    for path in sorted((data / "logs" / "brains").glob("seat-*.log")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if not text.startswith("Start Reader 1.0 (start-reader)"):
            continue
        lines[path.name] = STARTS.findall(text)
    return lines


def rows_of(text):
    """The name=start pairs of one starts line, as a dict."""
    rows = {}
    for part in text.split(", "):
        found = ROW.match(part)
        if found is None:
            raise ScriptFailure(f"the starts line holds {part!r}")
        rows[found.group(1)] = found.group(2)
    return rows


def require_same_starts(lines):
    """Both seats wrote one fixed line, and they name the same starts."""
    if len(lines) != 2:
        raise ScriptFailure(f"{len(lines)} seat logs of Start Reader, not two")
    seen = []
    for name, found in lines.items():
        if len(found) != 1:
            raise ScriptFailure(f"{name} holds {len(found)} starts lines, not one")
        rule, text = found[0]
        if rule != "fixed":
            raise ScriptFailure(f"{name} says the starts are {rule}, not fixed")
        seen.append((name, text))
    (first_name, first), (second_name, second) = seen
    if first != second:
        raise ScriptFailure(
            f"the machines give different starts: {first_name} {first!r}, "
            f"{second_name} {second!r}"
        )
    rows = rows_of(first)
    starts = sorted(rows.values())
    if len(rows) != 4 or starts != ["1", "2", "3", "4"]:
        raise ScriptFailure(f"the starts {first!r} are not 1 to 4, one for each player")
    return first


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("net brain starts: skipped, no installed game")
        return 77
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-net-brain-starts-", dir=scratch_root) as temporary:
        work = Path(temporary)
        user = work / "user"
        write_brain(user)
        preferences = work / "preferences.conf"
        write_preferences(preferences)
        data = work / "data"
        status, output = run_game(native, game_dir, work, [
            "--headless-check",
            "--net-loopback-check",
            "600",
            "--net-loopback-each-ai",
            "--net-loopback-fixed-starts",
            "--user-folder",
            str(user),
            "--data-dir",
            str(data),
            "--preferences-file",
            str(preferences),
        ])
        try:
            played = [line for line in output.splitlines() if PLAYS in line]
            if len(played) != 2:
                raise ScriptFailure(f"{len(played)} seats played Start Reader, not two")
            if BUILT not in output:
                raise ScriptFailure("the loopback did not report that the computer player built")
            if status != 0:
                raise ScriptFailure(f"open-annihilation exited with {status}")
            starts = require_same_starts(start_lines(data))
        except ScriptFailure as failure:
            print(output, end="" if output.endswith("\n") or output == "" else "\n")
            for name, found in start_lines(data).items():
                print(f"net brain starts: {name}: {found}")
            print(f"net brain starts: {failure}", flush=True)
            return 1
    print(f"net brain starts: both machines' brains read the fixed starts {starts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
