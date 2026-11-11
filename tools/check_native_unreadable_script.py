#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a unit script the game cannot read stops neither its unit nor the load.

A unit whose script file is not a COB the game reads plays without a
script, as one whose script file is absent does: the match loads every unit
type, says once that the unit plays without a script, and plays on.

The game starts a headless skirmish twice, with --combat on the dummy SDL
drivers: once on the installation --game-dir names, and once with
--mod-dir naming a scratch mod folder that holds only a script for the Solar
Collector whose header puts its code past the end of the file. Both must end
with status 0 and load as many unit types; the second must say once that
ARMSOLAR plays without a script, and neither may report a simulation error.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# The unit whose script the mod folder replaces: a building with no weapon.
UNIT = "ARMSOLAR"
# A COB header of eleven words whose three code words would run past its own
# 44 bytes: no table lies where the offsets say.
UNREADABLE_SCRIPT = struct.pack("<11I", 4, 0, 0, 3, 0, 0, 44, 44, 44, 44, 44)
NO_SCRIPT = re.compile(rf"^open-annihilation: invalid unit script scripts/{UNIT}\.COB: .+; "
                       rf"{UNIT} plays without a script$")
PREPARED = re.compile(r"^Offline match world prepared for .+ with (\d+) unit runtimes")
TICKS = "120"
RUN_TIMEOUT = 900


class ScriptFailure(Exception):
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


def skirmish(native, game_dir, workdir, name, mod_dir=None):
    """Plays the headless skirmish; returns its output lines."""
    _offline_preferences(workdir / f"{name}.conf")
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    command = [*RUNNER, str(native), "--game-dir", str(game_dir)]
    if mod_dir is not None:
        command += ["--mod-dir", str(mod_dir)]
    command += ["--skip-intro", "--mute", "--headless-check", "--match-ticks", TICKS,
                "--combat", "4", "--preferences-file", str(workdir / f"{name}.conf")]
    result = subprocess.run(command, cwd=workdir, env=environment, timeout=RUN_TIMEOUT, check=False,
                            capture_output=True, text=True, errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise ScriptFailure(f"open-annihilation exited with {result.returncode} on the {name} run")
    lines = output.splitlines()
    if any(line.startswith("simulation error") for line in lines):
        print(output, end="")
        raise ScriptFailure(f"the {name} run reported a simulation error")
    return lines


def unit_runtimes(lines, name):
    """Returns how many unit types the run's match loaded."""
    counts = [int(match.group(1)) for match in map(PREPARED.match, lines) if match]
    if len(counts) != 1:
        raise ScriptFailure(f"the {name} run prepared {len(counts)} matches instead of one")
    return counts[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-unreadable-unit-script-",
                                     dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        mod_dir = workdir / "mod"
        (mod_dir / "scripts").mkdir(parents=True)
        (mod_dir / "scripts" / f"{UNIT}.COB").write_bytes(UNREADABLE_SCRIPT)
        try:
            installed = skirmish(native, game_dir, workdir, "installation")
            unreadable = skirmish(native, game_dir, workdir, "unreadable", mod_dir)
            if any(NO_SCRIPT.match(line) for line in installed):
                raise ScriptFailure(f"the installation's {UNIT} played without a script")
            said = sum(1 for line in unreadable if NO_SCRIPT.match(line))
            if said != 1:
                print("\n".join(unreadable))
                raise ScriptFailure(f"the unreadable script was reported {said} times, not once")
            expected = unit_runtimes(installed, "installation")
            loaded = unit_runtimes(unreadable, "unreadable")
            if loaded != expected:
                raise ScriptFailure(f"{loaded} unit types loaded beside the unreadable script, not {expected}")
        except ScriptFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print(f"the unreadable unit script check passed: {expected} unit types, {UNIT} without a script")
    return 0


if __name__ == "__main__":
    sys.exit(main())
