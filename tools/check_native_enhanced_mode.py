#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check Enhanced Mode's opt-in rules and save lifetime in an isolated player folder."""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--game-dir", required=True, type=Path)
    args = parser.parse_args()
    environment = os.environ | {
        "SDL_VIDEO_DRIVER": "dummy",
        "SDL_AUDIO_DRIVER": "dummy",
        "SDL_RENDER_DRIVER": "software",
    }
    with tempfile.TemporaryDirectory(prefix="oa-enhanced-mode-") as temporary:
        root = Path(temporary)
        command = shlex.split(os.environ.get("OA_TEST_RUNNER", "")) + [
            str(args.native.resolve()), "--game-dir", str(args.game_dir.resolve()),
            "--headless-check", "--check-enhanced-mode", "--skip-intro", "--mute",
            "--preferences-file", str(root / "preferences.conf"),
        ]
        result = subprocess.run(command, cwd=root, env=environment, capture_output=True,
                                text=True, timeout=900)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        if "Enhanced Mode: preset, graphics independence" not in result.stdout:
            raise RuntimeError("the native check did not report success\n" + result.stdout)
        print(result.stdout)
        base = shlex.split(os.environ.get("OA_TEST_RUNNER", "")) + [
            str(args.native.resolve()), "--game-dir", str(args.game_dir.resolve()),
            "--headless-check", "--skip-intro", "--mute",
        ]
        recording = root / "enhanced.tad"
        record = subprocess.run(
            base + ["--enhanced-mode", "--net-loopback-check", "300", "--net-record",
                    str(recording), "--preferences-file", str(root / "record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if record.returncode or not recording.is_file():
            raise RuntimeError("enhanced network recording failed\n" + record.stdout + record.stderr)
        playback = subprocess.run(
            base + ["--no-enhanced-mode", "--play-demo", str(recording), "--match-ticks",
                    "300", "--preferences-file", str(root / "playback.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if playback.returncode or "tick errors 0" not in playback.stdout:
            raise RuntimeError("enhanced replay failed\n" + playback.stdout + playback.stderr)
        print("Enhanced Mode: network recording replays with the default set to Off")


if __name__ == "__main__":
    main()
