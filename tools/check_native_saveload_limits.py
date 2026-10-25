#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A save under a mod with more weapon types loads only under that mod.

Run A plays a headless skirmish under the mod directory and saves at tick 30.
When a stage file that places RIDGEW300 is already in the tree, run A uses
it. Run B loads that save under the same directory with --match-ticks 0 and
must print the same tick, units and digest. Run C loads it with no mod and
must be refused.

Every run uses --skip-intro --mute and a scratch user folder. The script
exits 77 when no game data is installed.
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

RUN_TIMEOUT_SECONDS = 600
SAVE_TICK = 30
DIGEST = re.compile(r"^saveload: tick (\d+) units (\d+) digest ([0-9a-f]{16})$", re.M)
REFUSED = "Savegame belongs to mod profile ridge-wide"
REPO = Path(__file__).resolve().parent.parent


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def ridge_stage(mod_dir):
    """T04's RIDGEW300 stage when the fixture or the tree already has one."""
    roots = [mod_dir, REPO / "tools", REPO / "tests", REPO / "src" / "app"]
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*.stage"):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "RIDGEW300" in text:
                return path
    return None


def launch(native, game_dir, work, user, *extra):
    """Runs one headless save or load. Returns the status and the combined log."""
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
        "--preferences-file",
        str(work / "preferences.conf"),
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


def digest_line(output):
    """The tick, units and digest the run printed, or nothing."""
    found = DIGEST.findall(output)
    if len(found) != 1:
        return None
    return found[0]


def require(condition, output, message):
    """Stops the check when a run did not do what it should."""
    if condition:
        return
    print(output, end="" if output.endswith("\n") or output == "" else "\n")
    raise ScriptFailure(message)


def prepare(root, name):
    """A scratch work directory, its user folder and an empty preferences file."""
    work = root / name
    user = work / "user"
    user.mkdir(parents=True)
    (work / "preferences.conf").write_text("open-annihilation-preferences 1\n")
    return work, user


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--mod-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("saveload limits: skipped, no installed game")
        return 77
    mod_dir = args.mod_dir.resolve()
    if not mod_dir.is_dir():
        raise ScriptFailure(f"{mod_dir} is not a mod directory")
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    stage = ridge_stage(mod_dir)
    with tempfile.TemporaryDirectory(prefix="T08-saveload-limits-", dir=scratch_root) as temporary:
        root = Path(temporary)
        save = root / "ridge-wide.sav"
        work_a, user_a = prepare(root, "run-a")
        extra = ["--mod-dir", str(mod_dir), "--match-ticks", str(SAVE_TICK),
                 "--save-after", str(SAVE_TICK), "--save-file", str(save)]
        if stage is not None:
            extra.extend(["--stage", str(stage)])
        status, output = launch(native, game_dir, work_a, user_a, *extra)
        require(status == 0, output, f"run A exited with {status}")
        saved = digest_line(output)
        require(saved is not None, output, "run A printed no saveload digest")
        require(saved[0] == str(SAVE_TICK), output, f"run A saved at tick {saved[0]}")
        require(save.is_file(), output, "run A wrote no save")
        require(saved[1] != "0", output, "run A saved a world without units")

        work_b, user_b = prepare(root, "run-b")
        status, output = launch(
            native, game_dir, work_b, user_b,
            "--mod-dir", str(mod_dir),
            "--load", str(save),
            "--match-ticks", "0",
        )
        require(status == 0, output, f"run B exited with {status}")
        loaded = digest_line(output)
        require(loaded == saved, output,
                f"run B printed {loaded}, not tick {saved[0]} units {saved[1]} digest {saved[2]}")

        work_c, user_c = prepare(root, "run-c")
        status, output = launch(
            native, game_dir, work_c, user_c,
            "--load", str(save),
            "--match-ticks", "0",
        )
        require(status != 0, output, "run C loaded a wide-mod save with no mod")
        require(REFUSED in output, output, "run C did not refuse the mod profile")
        require(digest_line(output) is None, output, "run C printed a saveload digest")
        print(
            f"saveload limits: ridge-wide saves and loads digest {saved[2]} "
            f"at tick {saved[0]} with {saved[1]} units; without the mod the save is refused"
        )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ScriptFailure as failure:
        raise SystemExit(f"saveload limits: {failure}") from None
