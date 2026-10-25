#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a unit fires a weapon past 255 at a unit and a feature.

Two headless skirmishes of one seed, over the mod directory, must each exit
0 and print the weapon-fire lines for the named type and weapon. The two
runs print the same lines and write byte-identical trace files.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 600
SEED = 4242


def fire_lines(text):
    return [line for line in text.splitlines() if line.startswith("weapon fire check:")]


def skirmish(native, game_dir, workdir, mod_dir, unit_type, trace):
    preferences = workdir / "preferences.conf"
    preferences.write_text("open-annihilation-preferences 1\n")
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--mod-dir",
        str(mod_dir),
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--preferences-file",
        str(preferences),
        "--match-ticks",
        str(TICKS),
        "--seed",
        str(SEED),
        "--weapon-fire-check",
        unit_type,
        "--trace-digest",
        str(trace),
    ]
    return subprocess.run(
        command,
        cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=600,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )


def require_ok(result, name):
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"{name} exited with {result.returncode}")
    return result.stdout + result.stderr


def require_weapon(lines, unit_type, weapon):
    text = "\n".join(lines)
    needed = (
        f"weapon fire check: {unit_type} fires weapon {weapon}",
        f"shots of weapon {weapon}",
        "weapon fire check: unit T took ",
        f"weapon fire check: weapon {weapon} hit the feature at ",
        "weapon fire check: tick ",
    )
    missing = [line for line in needed if line not in text]
    if missing:
        raise SystemExit(f"missing {missing} in:\n{text}")
    if " digest " not in text:
        raise SystemExit(f"missing the digest line in:\n{text}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--mod-dir", type=Path, required=True)
    parser.add_argument("--type", required=True)
    parser.add_argument("--weapon", required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    mod_dir = args.mod_dir.resolve()
    if not mod_dir.is_dir():
        raise SystemExit(f"{mod_dir} is not a directory")
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="native-weapon-fire-", dir=args.scratch_root.resolve()
    ) as temporary:
        root = Path(temporary)
        lines = []
        traces = []
        for index in (1, 2):
            workdir = root / f"run-{index}"
            workdir.mkdir()
            trace = workdir / "trace.bin"
            output = require_ok(
                skirmish(native, game_dir, workdir, mod_dir, args.type, trace),
                f"run {index}",
            )
            found = fire_lines(output)
            require_weapon(found, args.type, args.weapon)
            if not trace.is_file():
                raise SystemExit(f"run {index} wrote no trace")
            lines.append(found)
            traces.append(trace.read_bytes())
        if lines[0] != lines[1]:
            raise SystemExit(f"the runs printed different lines:\n{lines[0]}\n{lines[1]}")
        if traces[0] != traces[1]:
            raise SystemExit("the runs wrote different traces")
    print(
        f"native weapon fire: {args.type} fires weapon {args.weapon} twice, "
        "one digest and one trace"
    )


if __name__ == "__main__":
    main()
