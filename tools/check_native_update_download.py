#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One native check, several starts. Each start has its own folders, so a
# resumed package, a bad package and a quit do not share a download.

import argparse
import subprocess
import sys
from pathlib import Path

# A feed the check replaces. The address is never fetched.
DUMMY_FEED = "http://127.0.0.1:9/v1/engine/stable.json"
DUMMY_KEY = "test-key=ed25519:11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="


def preferences(path, background):
    """Writes the preferences one start opens with."""
    lines = [
        "open-annihilation-preferences 1",
        '"open-annihilation.language" "en"',
        '"open-annihilation.first-run" "done"',
    ]
    if background:
        lines.append('"open-annihilation.update-download" "1"')
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def run_case(native, game_dir, work, name, extra, needles, forbidden, status):
    """Runs one start and returns whether its output and status match."""
    folder = work / name
    user = folder / "user"
    data = folder / "data"
    install = (folder / "install").resolve()
    user.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    install.mkdir(parents=True, exist_ok=True)
    prefs = folder / "prefs.conf"
    preferences(prefs, name == "background")
    command = [
        native,
        "--game-dir",
        game_dir,
        "--skip-intro",
        "--mute",
        "--check-update-download",
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(prefs),
        "--update-feed",
        DUMMY_FEED,
        "--update-key",
        DUMMY_KEY,
        "--update-install-folder",
        str(install),
        *extra,
    ]
    # The case's own folder is its working folder: the pictures case writes there.
    completed = subprocess.run(
        command, cwd=str(folder), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    sys.stdout.write(completed.stdout)
    sys.stdout.flush()
    missing = [needle for needle in needles if needle not in completed.stdout]
    present = [needle for needle in forbidden if needle in completed.stdout]
    if completed.returncode == status and not missing and not present:
        return True
    print(
        f"update-download case {name}: status {completed.returncode}, "
        f"wanted {status}, missing {missing}, unexpected {present}",
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
    helper = "update-download: helper"
    cases = [
        (
            "resume",
            [],
            [
                "update started the download",
                "resumed with Range",
                "paused_for_match",
                "resumed after the match",
                "ready prompt queued",
                "status ready",
                "header note ready",
                "restart now",
                helper,
                "--plan",
                "plan.json",
                "--feed-key",
                "plan read back",
            ],
            [],
            0,
        ),
        (
            "corrupt",
            ["--update-download-case", "corrupt"],
            ["corrupt package deleted", "queued nothing"],
            [helper],
            0,
        ),
        (
            "updated",
            ["--updated-from", "0.0.1"],
            ["updated to", "updated once"],
            [helper],
            0,
        ),
        (
            "background",
            ["--update-download-case", "background"],
            ["no offer prompt", "ready prompt first"],
            [],
            0,
        ),
        (
            "update-and-quit",
            ["--update-and-quit"],
            ["update-and-quit", "no prompt", helper, "--plan"],
            [],
            0,
        ),
        (
            "no-offer",
            ["--update-and-quit", "--update-download-case", "no-offer"],
            ["no offer"],
            [helper],
            1,
        ),
        (
            "cancel",
            ["--update-download-case", "cancel"],
            [
                "pill paused and resumed",
                "update centre paused and resumed",
                "cancel download kept nothing",
            ],
            [helper],
            0,
        ),
        (
            "pictures",
            ["--update-download-case", "pictures"],
            ["update-settings-downloading-640x480.png", "pictures done"],
            [helper],
            0,
        ),
    ]
    failed = False
    for name, extra, needles, forbidden, status in cases:
        if not run_case(args.native, args.game_dir, work, name, extra, needles, forbidden, status):
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
