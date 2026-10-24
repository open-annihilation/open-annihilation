#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Pack brains/classic twice and compare the packages.

The two packs in a scratch folder must be byte for byte the same, and the
same as the package the build wrote beside the game. A non-zero pack status
fails the check.
"""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def pack(tool, source, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        dest.unlink()
    result = subprocess.run(
        [str(tool), "brain", "pack", "--force", "--out", str(dest), str(source)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"brain pack exited with {result.returncode}")
    if not dest.is_file():
        raise SystemExit(f"brain pack wrote no file at {dest}")
    return dest.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--beside", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    beside = args.beside.resolve()
    if not beside.is_file():
        raise SystemExit(f"the package beside the game is missing: {beside}")
    shipped = beside.read_bytes()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="C06-classic-package-", dir=args.scratch_root.resolve()
    ) as temporary:
        root = Path(temporary)
        first = pack(args.tool, args.source, root / "first.oabrain")
        second = pack(args.tool, args.source, root / "second.oabrain")
    if first != second:
        raise SystemExit("two packs of brains/classic differ")
    if first != shipped:
        raise SystemExit("the pack differs from the package beside the game")
    print(
        f"classic package: {len(first)} bytes, two packs match the file beside the game"
    )


if __name__ == "__main__":
    main()
