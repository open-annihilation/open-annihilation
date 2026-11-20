#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A saved game brings its mutators back, or names the one that is missing.

Run A plays a headless skirmish with ridge-tough installed in a scratch user
folder and saves after 30 ticks. Its log holds the applied line for
ridge-tough. Run B loads that save with no mutator chosen, and its log holds
the same line. Run D saves a game played without mutators, then loads that
save with ridge-tough chosen, and its log holds no applied line. Run E saves
Arm Campaign's first mission with ridge-tough and loads it with none chosen.
Run F plays the campaign navigation check with ridge-tough, which writes a
between-missions save, and loads that save: it loads while ridge-tough is
installed, and once the package is gone the load fails with the sentence
that names it. Run C removes ridge-tough and loads run A's save: the load
fails, the sentence names ridge-tough, and no match starts.

Every run uses --skip-intro --mute and the scratch user folder. The script
exits 77 when no game data is installed.
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

RUN_TIMEOUT_SECONDS = 900
SAVE_TICK = 30
APPLIED = "mutators: applied ridge-tough"
MISSING = "This needs a mutator you do not have:"
PACKAGE_ID = "ridge-tough"
PACKAGE = Path(__file__).resolve().parent.parent / "tests/data/mutators/packages" / PACKAGE_ID


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


def install_package(user):
    """Copies ridge-tough into the scratch Mutators folder."""
    destination = user / "Mutators" / PACKAGE_ID
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(PACKAGE, destination)


def launch(native, game_dir, work, user, *extra, timeout=RUN_TIMEOUT_SECONDS):
    """Runs one headless save or load. Returns the status and the combined log."""
    environment = dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        SDL_RENDER_DRIVER="software",
    )
    _offline_preferences(work / "preferences.conf")
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
        timeout=timeout,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    return result.returncode, result.stdout + result.stderr


def applied_line(output):
    """The line that says ridge-tough applied, or nothing."""
    found = [line for line in output.splitlines() if APPLIED in line]
    if len(found) != 1:
        return None
    return found[0]


def require(condition, output, message):
    """Stops the check when a run did not do what it should."""
    if condition:
        return
    print(output, end="" if output.endswith("\n") or output == "" else "\n")
    raise ScriptFailure(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    if not game_dir.is_dir():
        print("saveload mutators: skipped, no installed game")
        return 77
    if not PACKAGE.is_dir():
        raise ScriptFailure(f"{PACKAGE} is not a mutator package")
    native = args.native.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-saveload-mutators-", dir=scratch_root) as temporary:
        work = Path(temporary)
        user = work / "user"
        install_package(user)
        mutated = work / "with-mutators.sav"
        plain = work / "without-mutators.sav"

        status, output = launch(
            native, game_dir, work, user,
            "--mutator", PACKAGE_ID,
            "--match-ticks", str(SAVE_TICK),
            "--save-after", str(SAVE_TICK),
            "--save-file", str(mutated),
        )
        require(status == 0 and mutated.is_file(), output, f"run A exited with {status}")
        played = applied_line(output)
        require(played is not None, output, "run A did not apply ridge-tough")
        print(f"saveload mutators: run A {played}")

        status, output = launch(
            native, game_dir, work, user,
            "--load", str(mutated),
            "--match-ticks", "0",
        )
        require(status == 0, output, f"run B exited with {status}")
        require(played in output.splitlines(), output, "run B did not apply the saved mutator")
        print(f"saveload mutators: run B {played}")

        status, output = launch(
            native, game_dir, work, user,
            "--match-ticks", str(SAVE_TICK),
            "--save-after", str(SAVE_TICK),
            "--save-file", str(plain),
        )
        require(status == 0 and plain.is_file(), output, f"run D's save exited with {status}")
        require(APPLIED not in output and "mutators: applied" not in output, output,
                "run D's save applied a mutator")
        status, output = launch(
            native, game_dir, work, user,
            "--load", str(plain),
            "--match-ticks", "0",
            "--mutator", PACKAGE_ID,
        )
        require(status == 0, output, f"run D's load exited with {status}")
        require("mutators: applied" not in output, output, "run D applied a mutator to a plain save")
        print("saveload mutators: run D loaded a plain save with none")

        campaign_save = work / "ac01.sav"
        status, output = launch(
            native, game_dir, work, user,
            "--campaign", "Arm Campaign",
            "--mission", "0",
            "--mutator", PACKAGE_ID,
            "--match-ticks", "280",
            "--save-after", "280",
            "--save-file", str(campaign_save),
        )
        require(
            status == 0 and campaign_save.is_file(), output, f"run E's save exited with {status}"
        )
        campaign_played = applied_line(output)
        require(campaign_played is not None, output, "run E's save did not apply ridge-tough")
        print(f"saveload mutators: run E {campaign_played}")
        status, output = launch(
            native, game_dir, work, user,
            "--load", str(campaign_save),
            "--match-ticks", "0",
        )
        require(status == 0, output, f"run E's load exited with {status}")
        require(
            campaign_played in output.splitlines(), output, "run E did not apply the saved mutator"
        )
        print(f"saveload mutators: run E load {campaign_played}")

        status, output = launch(
            native, game_dir, work, user,
            "--check-navigation", "campaign",
            "--mutator", PACKAGE_ID,
            timeout=RUN_TIMEOUT_SECONDS,
        )
        require(status == 0, output, f"run F's campaign exited with {status}")
        # The saves themselves; each has a .oa-content note beside it.
        between = sorted(
            path for path in (user / "Saves").rglob("NAVBETWEEN*")
            if path.is_file() and path.suffix.upper() == ".SAV"
        )
        kept = [
            path for path in between
            if b"oaloadout" in path.read_bytes() and b"ridge-tough" in path.read_bytes()
        ]
        names = ", ".join(path.name for path in between) or "none"
        require(
            kept, output, f"run F's between-missions saves ({names}) kept no ridge-tough loadout"
        )
        between_save = next((path for path in kept if "存档" in path.name), kept[0])
        status, output = launch(
            native, game_dir, work, user,
            "--load", str(between_save),
            "--match-ticks", "0",
        )
        require(status == 0 and "briefing" in output, output, f"run F's load exited with {status}")
        print(f"saveload mutators: run F loaded {between_save.name}")

        shutil.rmtree(user / "Mutators" / PACKAGE_ID)
        status, output = launch(
            native, game_dir, work, user,
            "--load", str(mutated),
            "--match-ticks", "0",
        )
        require(status != 0, output, "run C loaded a save whose mutator is missing")
        require(MISSING in output and PACKAGE_ID in output, output,
                "run C did not name ridge-tough")
        require("mutators: applied" not in output and "saveload: tick" not in output, output,
                "run C started a match")
        sentence = next(
            (line for line in output.splitlines() if MISSING in line and PACKAGE_ID in line),
            "",
        )
        print(f"saveload mutators: run C {sentence}")

        status, output = launch(
            native, game_dir, work, user,
            "--load", str(between_save),
            "--match-ticks", "0",
        )
        require(
            status != 0, output, "run F loaded a between-missions save whose mutator is missing"
        )
        require(
            MISSING in output and PACKAGE_ID in output, output, "run F did not name ridge-tough"
        )
        require(
            "mutators: applied" not in output and "saveload: tick" not in output,
            output,
            "run F started a match",
        )
        missing = next(
            (line for line in output.splitlines() if MISSING in line and PACKAGE_ID in line),
            "",
        )
        print(f"saveload mutators: run F missing {missing}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ScriptFailure as failure:
        raise SystemExit(str(failure)) from None
