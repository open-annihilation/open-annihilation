#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the walk of SDL's render drivers in the game's log.

The game starts three times with --skip-intro --mute --frames 1 on the dummy
SDL drivers, which have no graphics device, so that every hardware render
driver refuses and only SDL's software renderer starts:

- with SDL_RENDER_DRIVER unset the game walks SDL's drivers in SDL's order
  (app-render-host checks the order against SDL's own list). Every driver
  tried before the software renderer must refuse and be logged once,
  as "open-annihilation: graphics: renderer <driver> refused: <reason>"
  with a reason, before the start-up line, which must name the software
  renderer on the dummy video driver; the software renderer must not be
  refused, and no second walk is logged, since no driver is recorded as
  failed. The start must end with status 0;
- with SDL_RENDER_DRIVER=software the start is SDL's own call, with no
  walk: no refusal is logged, the start-up line is, and the start ends with
  status 0;
- with SDL_RENDER_DRIVER naming a driver that does not exist the start
  fails as it always has: status 1, the error
  "open-annihilation: SDL_CreateRenderer: <reason>", no refusal logged and
  no start-up line.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
PREFIX = "open-annihilation: graphics: "
# A refused driver's line: its name and the reason.
REFUSAL = re.compile(re.escape(PREFIX) + r"renderer (\S+) refused: (.*)$")
# The line a second walk from the top logs.
SECOND_WALK = PREFIX + "no driver left could present"
# The line a start on SDL's software renderer and the dummy video driver logs.
# The named preferences file leaves Hardware acceleration Off.
STARTED = (PREFIX + "software on dummy, textures of any size; "
           "standard tier: the processor draws everything (hardware acceleration is off)")
SOFTWARE = "software"
# The error a start that made no renderer ends with.
FAILED = "open-annihilation: SDL_CreateRenderer: "
# A render driver no SDL has.
MISSING_DRIVER = "missing"
FRAMES = "1"
RUN_TIMEOUT = 900



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


def start(args, workdir, name, render_driver):
    """Start the game once; return its exit status and its output's lines."""
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    environment.pop("SDL_RENDER_DRIVER", None)
    environment.pop("SDL_FRAMEBUFFER_ACCELERATION", None)
    if render_driver is not None:
        environment["SDL_RENDER_DRIVER"] = render_driver
    _offline_preferences(workdir / f"{name}.conf")
    result = subprocess.run(
        [*RUNNER, str(args.native.resolve()), "--game-dir", str(args.game_dir.resolve()),
         "--skip-intro", "--mute", "--frames", FRAMES,
         "--preferences-file", str(workdir / f"{name}.conf")],
        cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")
    return result.returncode, result.stdout.splitlines(), result.stderr.splitlines()


def check_walk(status, lines):
    """Return the failures of the start with no driver named."""
    failures = []
    if status != 0:
        failures.append(f"exited with {status}")
    refused = []
    for line in lines:
        match = REFUSAL.match(line)
        if match is None:
            continue
        driver, reason = match.groups()
        if not reason.strip():
            failures.append(f"{driver}'s refusal gives no reason")
        refused.append(driver)
    if not refused:
        failures.append("no render driver was logged as refused before the software renderer")
    if len(set(refused)) != len(refused):
        failures.append(f"a driver was refused twice: {refused}")
    if SOFTWARE in refused:
        failures.append("the software renderer was refused")
    if lines.count(STARTED) != 1:
        failures.append(f"the start logged {lines.count(STARTED)} lines reading {STARTED!r}, not one")
    else:
        started_at = lines.index(STARTED)
        if any(REFUSAL.match(line) for line in lines[started_at + 1:]):
            failures.append("a refusal was logged after the start-up line")
    if any(line.startswith(SECOND_WALK) for line in lines):
        failures.append("a second walk was logged with no driver recorded as failed")
    return failures, refused


def check_named_software(status, lines):
    """Return the failures of the start with the software renderer named."""
    failures = []
    if status != 0:
        failures.append(f"exited with {status}")
    if any(REFUSAL.match(line) for line in lines):
        failures.append("a refusal was logged, so the drivers were walked")
    if lines.count(STARTED) != 1:
        failures.append(f"the start logged {lines.count(STARTED)} lines reading {STARTED!r}, not one")
    return failures


def check_named_missing(status, lines):
    """Return the failures of the start with a driver named that does not exist."""
    failures = []
    if status != 1:
        failures.append(f"exited with {status}, not 1")
    if not any(line.startswith(FAILED) for line in lines):
        failures.append(f"no line begins {FAILED!r}")
    if any(REFUSAL.match(line) for line in lines):
        failures.append("a refusal was logged, so the drivers were walked")
    if STARTED in lines:
        failures.append("the start-up line was logged")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    failed = False
    with tempfile.TemporaryDirectory(prefix="native-renderer-walk-",
                                     dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        # Standard output alone keeps the walk's lines in the order logged.
        status, out, err = start(args, workdir, "walk", None)
        failures, refused = check_walk(status, out)
        runs = [("no driver named", failures, out + err)]
        status, out, err = start(args, workdir, "named-software", SOFTWARE)
        runs.append(("software named", check_named_software(status, out), out + err))
        status, out, err = start(args, workdir, "named-missing", MISSING_DRIVER)
        runs.append(("a missing driver named", check_named_missing(status, out + err), out + err))
    for label, failures, lines in runs:
        if failures:
            failed = True
            print("\n".join(lines))
            for failure in failures:
                print(f"FAIL {label}: {failure}")
    if failed:
        return 1
    print(f"refused before the software renderer, in order: {', '.join(refused)}")
    print("the renderer walk check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
