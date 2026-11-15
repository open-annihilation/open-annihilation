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


def recording_rules_version(path):
    """Reads the one Enhanced Mode sector from a recording's chunked header."""
    with path.open("rb") as stream:
        def chunk():
            length_bytes = stream.read(2)
            length = int.from_bytes(length_bytes, "little")
            if len(length_bytes) != 2 or length < 2:
                raise RuntimeError(f"{path.name}: invalid recording chunk length")
            body = stream.read(length - 2)
            if len(body) != length - 2:
                raise RuntimeError(f"{path.name}: truncated recording chunk")
            return body

        if not chunk().startswith(b"TA Demo\0"):
            raise RuntimeError(f"{path.name}: missing recording header")
        count_bytes = chunk()
        if len(count_bytes) != 4:
            raise RuntimeError(f"{path.name}: invalid sector count")
        count = int.from_bytes(count_bytes, "little")
        if count > 256:
            raise RuntimeError(f"{path.name}: too many recording sectors")
        versions = []
        for _ in range(count):
            sector = chunk()
            if len(sector) < 4:
                raise RuntimeError(f"{path.name}: missing sector type")
            if int.from_bytes(sector[:4], "little") == 0x4F414531:
                if len(sector) != 8:
                    raise RuntimeError(f"{path.name}: invalid Enhanced Mode sector")
                versions.append(int.from_bytes(sector[4:], "little"))
        if len(versions) != 1:
            raise RuntimeError(f"{path.name}: expected one Enhanced Mode sector")
        return versions[0]


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
        cohort_recording = root / "enhanced-v10.tad"
        cohort_record = subprocess.run(
            base + ["--enhanced-mode-version", "10", "--net-loopback-check", "300",
                    "--net-record", str(cohort_recording), "--preferences-file",
                    str(root / "cohort-record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if cohort_record.returncode or not cohort_recording.is_file():
            raise RuntimeError("v10 network recording failed\n" +
                               cohort_record.stdout + cohort_record.stderr)
        if recording_rules_version(cohort_recording) != 10:
            raise RuntimeError("the CLI v10 recording did not record rules version 10")
        cohort_playback = subprocess.run(
            base + ["--enhanced-mode-version", "8", "--play-demo", str(cohort_recording),
                    "--match-ticks", "300", "--preferences-file",
                    str(root / "cohort-playback.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if cohort_playback.returncode or "tick errors 0" not in cohort_playback.stdout:
            raise RuntimeError("v10 replay under CLI v8 failed\n" +
                               cohort_playback.stdout + cohort_playback.stderr)
        print("Enhanced Mode: CLI v10 recording keeps its rules under a CLI v8 selection")
        retained_recording = root / "enhanced-v11.tad"
        retained_record = subprocess.run(
            base + ["--enhanced-mode-version", "11", "--net-loopback-check", "300",
                    "--net-record", str(retained_recording), "--preferences-file",
                    str(root / "retained-record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if retained_record.returncode or not retained_recording.is_file():
            raise RuntimeError("v11 network recording failed\n" +
                               retained_record.stdout + retained_record.stderr)
        if recording_rules_version(retained_recording) != 11:
            raise RuntimeError("the CLI v11 recording did not record rules version 11")
        retained_playback = subprocess.run(
            base + ["--enhanced-mode-version", "10", "--play-demo", str(retained_recording),
                    "--match-ticks", "300", "--preferences-file",
                    str(root / "retained-playback.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if retained_playback.returncode or "tick errors 0" not in retained_playback.stdout:
            raise RuntimeError("v11 replay under CLI v10 failed\n" +
                               retained_playback.stdout + retained_playback.stderr)
        print("Enhanced Mode: CLI v11 recording keeps its rules under a CLI v10 selection")
        cardinal_recording = root / "enhanced-v13.tad"
        cardinal_record = subprocess.run(
            base + ["--enhanced-mode-version", "13", "--net-loopback-check", "300",
                    "--net-record", str(cardinal_recording), "--preferences-file",
                    str(root / "cardinal-record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if cardinal_record.returncode or not cardinal_recording.is_file():
            raise RuntimeError("v13 network recording failed\n" +
                               cardinal_record.stdout + cardinal_record.stderr)
        if recording_rules_version(cardinal_recording) != 13:
            raise RuntimeError("the CLI v13 recording did not record rules version 13")
        cardinal_playback = subprocess.run(
            base + ["--enhanced-mode-version", "10", "--play-demo", str(cardinal_recording),
                    "--match-ticks", "300", "--preferences-file",
                    str(root / "cardinal-playback.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if cardinal_playback.returncode or "tick errors 0" not in cardinal_playback.stdout:
            raise RuntimeError("v13 replay under CLI v10 failed\n" +
                               cardinal_playback.stdout + cardinal_playback.stderr)
        print("Enhanced Mode: CLI v13 recording keeps its rules under a CLI v10 selection")


if __name__ == "__main__":
    main()
