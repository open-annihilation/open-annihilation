#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check Enhanced Mode's opt-in rules and save lifetime in an isolated player folder.

A battle room plays revisions only, so the network recording here plays
Revision 1, and the old presets 10, 11 and 13 are recorded from skirmishes.
"""

import argparse
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import native_offline


def sector_bits(feature_ids):
    """Bit (id − 1) for each feature id, the recording sector's layout."""
    bits = 0
    for feature_id in feature_ids:
        bits |= 1 << (feature_id - 1)
    return bits


# Alias sets for versions 10, 11 and 13, feature ids in id order.
# Version 10 is version 8 plus cohort targets and steering-projection reuse.
# Version 11 adds cohort retention. Version 13 adds cardinal queue escape
# and does not include cohort retention.
ALIAS_FEATURES = {
    10: [1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25],
    11: [1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26],
    13: [1, 2, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27],
}
# Revision 1's feature ids: capacity, local_flow, routes_fine_regions,
# route_corner_advance, ai_strike_retarget, credited_fallback_search,
# ai_attack_retention, ai_rally_retention, occupancy_free_planning,
# steering_projection_reuse, exact_indexed_searches, wide_strike_recruits,
# paced_cohort_targets, dispatch_cohort_retention, counterflow_queue_escape,
# idle_goal_skip and static_interior_refresh_skip.
REVISION_1_FEATURES = [1, 2, 5, 7, 11, 18, 20, 21, 23, 25, 28, 29, 30, 31, 32, 33, 34]
# 0.9.3 packed as major << 16 | minor << 8 | patch.
NEEDED_ENGINE = 0x00000903


def recording_enhanced(path):
    """Reads the one Enhanced Mode sector: revision, feature bits, needed version."""
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
        found = []
        for _ in range(count):
            sector = chunk()
            if len(sector) < 4:
                raise RuntimeError(f"{path.name}: missing sector type")
            if int.from_bytes(sector[:4], "little") == 0x4F414531:
                # The type word plus 16 little-endian bytes: revision, bits, version.
                if len(sector) != 20:
                    raise RuntimeError(f"{path.name}: invalid Enhanced Mode sector")
                body = sector[4:]
                found.append((
                    int.from_bytes(body[0:4], "little"),
                    int.from_bytes(body[4:12], "little"),
                    int.from_bytes(body[12:16], "little"),
                ))
        if len(found) != 1:
            raise RuntimeError(f"{path.name}: expected one Enhanced Mode sector")
        return found[0]


def require_alias_sector(path, version):
    """The recording stores revision 1, that version's feature bits and engine 0.9.3."""
    recorded = recording_enhanced(path)
    expected = (1, sector_bits(ALIAS_FEATURES[version]), NEEDED_ENGINE)
    if recorded != expected:
        raise RuntimeError(
            f"{path.name}: Enhanced Mode sector {recorded} is not alias {version} {expected}"
        )


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
        refusal_log = result.stdout + result.stderr
        for line in (
            "This game needs Open Annihilation 0.9.4 or later.",
            "This game needs a later version of Open Annihilation.",
        ):
            if line not in refusal_log:
                raise RuntimeError("the native check did not log a refusal\n" + refusal_log)
            # The game writes these on stderr. Repeat them so the test log
            # keeps the two sentences the check required.
            print(line)
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
        # A network game plays a revision, never a preset of its own.
        network = recording_enhanced(recording)
        if network != (1, sector_bits(REVISION_1_FEATURES), NEEDED_ENGINE):
            raise RuntimeError(f"{recording.name}: Enhanced Mode sector {network} is not Revision 1")
        playback = subprocess.run(
            base + ["--no-enhanced-mode", "--play-demo", str(recording), "--match-ticks",
                    "300", "--preferences-file", str(root / "playback.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if playback.returncode or "tick errors 0" not in playback.stdout:
            raise RuntimeError("enhanced replay failed\n" + playback.stdout + playback.stderr)
        print("Enhanced Mode: Revision 1 network recording replays with the default set to Off")
        cohort_recording = root / "enhanced-v10.tad"
        cohort_record = subprocess.run(
            base + ["--enhanced-mode-version", "10", "--give-orders", "--seed", "7",
                    "--match-ticks", "300", "--record-game", str(cohort_recording),
                    "--preferences-file", str(root / "cohort-record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if cohort_record.returncode or not cohort_recording.is_file():
            raise RuntimeError("v10 skirmish recording failed\n" +
                               cohort_record.stdout + cohort_record.stderr)
        require_alias_sector(cohort_recording, 10)
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
            base + ["--enhanced-mode-version", "11", "--give-orders", "--seed", "7",
                    "--match-ticks", "300", "--record-game", str(retained_recording),
                    "--preferences-file", str(root / "retained-record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if retained_record.returncode or not retained_recording.is_file():
            raise RuntimeError("v11 skirmish recording failed\n" +
                               retained_record.stdout + retained_record.stderr)
        require_alias_sector(retained_recording, 11)
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
            base + ["--enhanced-mode-version", "13", "--give-orders", "--seed", "7",
                    "--match-ticks", "300", "--record-game", str(cardinal_recording),
                    "--preferences-file", str(root / "cardinal-record.conf")],
            cwd=root, env=environment, capture_output=True, text=True, timeout=900,
        )
        if cardinal_record.returncode or not cardinal_recording.is_file():
            raise RuntimeError("v13 skirmish recording failed\n" +
                               cardinal_record.stdout + cardinal_record.stderr)
        require_alias_sector(cardinal_recording, 13)
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
