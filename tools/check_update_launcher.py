#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The Linux update launcher swaps next over current and starts that version.

A scratch install stands in for the game. Each versions folder holds a shell
script named open-annihilation that writes its arguments to a file.
"""

import os
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path


def stand_in(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#!/bin/sh\nprintf \'%s\\n\' "$@" > "$OA_V10_ARGS"\n', encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def start(launcher: Path, args_file: Path, args: list[str]) -> None:
    env = os.environ.copy()
    env["OA_V10_ARGS"] = str(args_file)
    if args_file.exists():
        args_file.unlink()
    result = subprocess.run(
        ["sh", str(launcher), *args],
        env=env,
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            "launcher failed\n" + result.stdout + result.stderr
        )


def link_target(path: Path) -> str:
    return os.readlink(path)


def with_next(root: Path, launcher_src: Path) -> None:
    install = root / "with-next"
    stand_in(install / "versions" / "0.8.6" / "open-annihilation")
    stand_in(install / "versions" / "0.8.7" / "open-annihilation")
    os.symlink("versions/0.8.6", install / "current")
    os.symlink("versions/0.8.7", install / "next")
    launcher = install / "open-annihilation"
    shutil.copy(launcher_src, launcher)
    launcher.chmod(0o755)
    args_file = root / "with-next-args"
    start(launcher, args_file, ["--skip-intro"])
    if link_target(install / "current") != "versions/0.8.7":
        raise SystemExit("current did not move to the new version")
    if (install / "next").exists() or (install / "next").is_symlink():
        raise SystemExit("next is still there")
    lines = args_file.read_text(encoding="utf-8").splitlines()
    if lines != ["--skip-intro", "--updated-from", "0.8.6"]:
        raise SystemExit("arguments were " + repr(lines))


def without_next(root: Path, launcher_src: Path) -> None:
    install = root / "no-next"
    stand_in(install / "versions" / "0.8.6" / "open-annihilation")
    os.symlink("versions/0.8.6", install / "current")
    launcher = install / "open-annihilation"
    shutil.copy(launcher_src, launcher)
    launcher.chmod(0o755)
    args_file = root / "no-next-args"
    start(launcher, args_file, ["hello"])
    if link_target(install / "current") != "versions/0.8.6":
        raise SystemExit("current changed when next was absent")
    lines = args_file.read_text(encoding="utf-8").splitlines()
    if lines != ["hello"]:
        raise SystemExit("arguments were " + repr(lines))


def missing_current(root: Path, launcher_src: Path) -> None:
    install = root / "no-current"
    stand_in(install / "versions" / "0.8.7" / "open-annihilation")
    launcher = install / "open-annihilation"
    shutil.copy(launcher_src, launcher)
    launcher.chmod(0o755)
    args_file = root / "no-current-args"
    start(launcher, args_file, ["--mute"])
    if link_target(install / "current") != "versions/0.8.7":
        raise SystemExit("current was not made")
    lines = args_file.read_text(encoding="utf-8").splitlines()
    if lines != ["--mute"]:
        raise SystemExit("arguments were " + repr(lines))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_update_launcher.py LAUNCHER")
    launcher_src = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="oa-v10-launcher-") as tmp:
        root = Path(tmp)
        with_next(root, launcher_src)
        without_next(root, launcher_src)
        missing_current(root, launcher_src)


if __name__ == "__main__":
    main()
