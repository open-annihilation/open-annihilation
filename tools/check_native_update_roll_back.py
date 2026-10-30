#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One native check, several starts. Each start has its own folders and its
# own state file, written before the game runs.

import argparse
import json
import subprocess
import sys
from pathlib import Path


def preferences(path):
    """Writes the preferences one start opens with."""
    lines = [
        "open-annihilation-preferences 1",
        '"open-annihilation.language" "en"',
        '"open-annihilation.first-run" "done"',
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def engine_version(native):
    """The version this build names, without a label after a hyphen."""
    completed = subprocess.run(
        [native, "--version"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    line = completed.stdout.strip().splitlines()[-1] if completed.stdout else ""
    parts = line.split()
    if completed.returncode != 0 or len(parts) < 3 or parts[0] != "Open" or parts[1] != "Annihilation":
        print(f"update-roll-back: --version did not name a build: {line}", file=sys.stderr)
        return ""
    token = parts[2]
    hyphen = token.find("-")
    if hyphen >= 0:
        token = token[:hyphen]
    return token


def write_state(folder, version, confirmed, failed_starts, kept):
    """Writes `<data>/updates/state.json` before the start."""
    updates = folder / "data" / "updates"
    updates.mkdir(parents=True, exist_ok=True)
    if kept:
        kept_path = str((folder / "kept").resolve())
        copy = str((folder / "preferences-copy.conf").resolve())
        kept_version = "0.8.6"
    else:
        kept_path = ""
        copy = ""
        kept_version = ""
    state = {
        "installed_version": version,
        "installed_release": 1,
        "kept_version": kept_version,
        "kept_path": kept_path,
        "kept_route": "check-folder",
        "preferences_copy": copy,
        "confirmed": confirmed,
        "failed_starts": failed_starts,
    }
    (updates / "state.json").write_text(json.dumps(state) + "\n", encoding="utf-8")


def run_case(native, game_dir, work, name, version, extra, needles):
    """Runs one start and returns whether its output and status match."""
    folder = work / name
    user = folder / "user"
    data = folder / "data"
    user.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    prefs = folder / "prefs.conf"
    preferences(prefs)
    if name == "guard":
        write_state(folder, version, False, 2, True)
    elif name == "confirm":
        write_state(folder, version, False, 1, True)
    elif name == "prompt":
        write_state(folder, version, True, 0, False)
    else:
        write_state(folder, version, True, 0, True)
    command = [
        native,
        "--game-dir",
        game_dir,
        "--skip-intro",
        "--mute",
        "--check-update-roll-back",
        "--roll-back-case",
        name,
        "--user-folder",
        str(user),
        "--data-dir",
        str(data.resolve()),
        "--preferences-file",
        str(prefs.resolve()),
        *extra,
    ]
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sys.stdout.write(completed.stdout)
    sys.stdout.flush()
    missing = [needle for needle in needles if needle not in completed.stdout]
    if completed.returncode == 0 and not missing:
        return True
    print(
        f"update-roll-back case {name}: status {completed.returncode}, "
        f"wanted 0, missing {missing}",
        file=sys.stderr,
    )
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    version = engine_version(args.native)
    if not version:
        return 1
    cases = [
        ("guard", [], ["update-roll-back: guard left 2"]),
        ("confirm", [], ["update-roll-back: confirmed"]),
        (
            "prompt",
            ["--rolled-back-from", "0.0.2", "--roll-back-reason", "failed-starts"],
            ["update-roll-back: back on", "update-roll-back: log shown"],
        ),
        ("settings", [], ["update-roll-back: plan roll-back"]),
    ]
    failed = 0
    for name, extra, needles in cases:
        if not run_case(args.native, args.game_dir, work, name, version, extra, needles):
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
