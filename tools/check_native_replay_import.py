#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check opening a replay: --check-replay-import.

The game copies a made-up .oademo and a made-up .tad into Recordings/Imported,
selects each in the Library, and says so. Opening either again copies nothing.
The imported .tad holds no sector 6. A file that is not a TA Demo recording,
and one in a version this game does not play, are each refused with that
sentence.

The check clears Recordings as it starts and again as it ends, and the script
gives it a user folder of its own, so a second run starts from the same state.
Two recordings given together are each announced, and the last one is selected.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds a run may take.
RUN_TIMEOUT = 600

COPIED = "replay import: copied "
SELECTED = "replay import: selected "
KEPT = "replay import: kept "
SECTOR = "replay import: tad-sector-6 0"
REFUSED_DAMAGED = "replay import: refused This file is not a TA Demo recording"
REFUSED_VERSION = (
    "replay import: refused This recording is in version 3 of the format, "
    "which Open Annihilation does not play"
)
BOTH_COAST = "replay import: both Added Coast to Replays"
BOTH_PLAIN = "replay import: both Added Plain to Replays"
BOTH_SELECTED = "replay import: both-selected Imported/plain.tad"


class ImportFailure(Exception):
    """A behaviour that differs from the expected one."""


def environment():
    return dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                SDL_RENDER_DRIVER="software")


def run(native, game_dir, workdir):
    """Runs the import check once; returns the finished process."""
    arguments = [
        "--game-dir", str(game_dir), "--skip-intro", "--mute",
        "--preferences-file", str(workdir / "import.conf"),
        "--user-folder", str(workdir / "user"),
        "--check-replay-import",
    ]
    result = subprocess.run(
        [*RUNNER, str(native), *arguments], cwd=workdir, env=environment(), timeout=RUN_TIMEOUT,
        check=False, capture_output=True, text=True, errors="replace")
    (workdir / "import.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
    return result


def require_lines(text, workdir):
    """The lines the check prints, in the order it prints them."""
    wanted = [
        COPIED, SELECTED, COPIED, SELECTED, SECTOR, KEPT, KEPT, REFUSED_DAMAGED, REFUSED_VERSION,
        BOTH_COAST, BOTH_PLAIN, BOTH_SELECTED,
    ]
    at = 0
    counts = {COPIED: 0, SELECTED: 0, KEPT: 0}
    for line in text.splitlines():
        if at < len(wanted) and line.startswith(wanted[at]):
            if wanted[at] in counts:
                counts[wanted[at]] += 1
            at += 1
    if at != len(wanted):
        raise ImportFailure(
            f"the run printed {at} of the {len(wanted)} expected lines; see {workdir / 'import.txt'}"
        )
    for kind, count in counts.items():
        if count != 2:
            raise ImportFailure(f"{kind!r} was printed {count} times; see {workdir / 'import.txt'}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if not args.game_dir.is_dir():
        print(f"native-replays-import: no installed game at {args.game_dir}; skipped")
        return 77
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix="replay-import-", dir=args.scratch_root))
    try:
        result = run(args.native, args.game_dir, workdir)
        if result.returncode != 0:
            raise ImportFailure(f"the game ended with status {result.returncode}")
        require_lines(result.stdout, workdir)
    except (ImportFailure, subprocess.TimeoutExpired, OSError) as failure:
        print(f"native-replays-import: {failure}; the run is in {workdir}", file=sys.stderr)
        return 1
    shutil.rmtree(workdir, ignore_errors=True)
    print("native-replays-import: a .oademo and a .tad were copied and selected, opening either "
          "again copied nothing, the .tad held no sector 6, and a damaged file was refused")
    return 0


if __name__ == "__main__":
    sys.exit(main())
