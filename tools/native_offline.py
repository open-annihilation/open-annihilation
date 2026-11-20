#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Offline preferences and a private data folder for a script that starts the game.

Importing this module wraps subprocess.run and subprocess.Popen. A command
that starts the game gains the flags from flags() when it does not already
carry them. A preferences file that is already there is left as the script
wrote it, so a loopback feed can keep the update check on. A file that is
absent is written with the update check at never and the catalogue check at
never, or at library when the command enters the Boneyards with -y.
"""

import subprocess
from pathlib import Path

_RUN = subprocess.run
_POPEN = subprocess.Popen

_HEADER = "open-annihilation-preferences 1\n"
_UPDATE = '"open-annihilation.update-check" "never"\n'
_CONTENT_NEVER = '"open-annihilation.content-updates" "never"\n'
_CONTENT_LIBRARY = '"open-annihilation.content-updates" "library"\n'
_LEAVE_UPDATE = (
    "--update-feed",
    "--check-engine-feed",
    "--check-update-offer",
    "--check-update-download",
    "--check-first-run",
    "--check-setup-window",
)


def preferences_text(catalogue="never"):
    """The preferences text for a launch that must not fetch a catalogue."""
    content = _CONTENT_LIBRARY if catalogue == "library" else _CONTENT_NEVER
    return _HEADER + _UPDATE + content


def ensure(path, catalogue="never"):
    """Writes the offline preferences when the file is absent. Returns the path."""
    target = Path(path)
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(preferences_text(catalogue), encoding="utf-8")
    return target


def flags(preferences, data_dir, catalogue="never"):
    """The --preferences-file and --data-dir arguments for one launch.

    The preferences file is written when it is absent. The data folder is
    created. catalogue is never or library.
    """
    ensure(preferences, catalogue)
    data = Path(data_dir)
    data.mkdir(parents=True, exist_ok=True)
    return ["--preferences-file", str(preferences), "--data-dir", str(data)]


def _text(command):
    if isinstance(command, (str, bytes)):
        return None
    try:
        return [str(argument) for argument in command]
    except TypeError:
        return None


def _game(arguments):
    if "--game-dir" in arguments or "--fark" in arguments or "--headless-check" in arguments:
        return True
    if not arguments:
        return False
    return Path(arguments[0]).name in ("open-annihilation", "open-annihilation.exe")


def _catalogue(arguments):
    if "-y" in arguments or any("enter-boneyards" in argument for argument in arguments):
        return "library"
    return "never"


def _last_flag(arguments, flag):
    """The argument after the last flag. A later flag replaces an earlier one."""
    value = None
    for index, argument in enumerate(arguments):
        if argument == flag and index + 1 < len(arguments):
            value = arguments[index + 1]
    return value


def apply(command):
    """Returns the command with offline flags added when it starts the game."""
    arguments = _text(command)
    if arguments is None or not _game(arguments):
        return command
    if "--version" in arguments and "--game-dir" not in arguments:
        return command
    if any(flag in arguments for flag in ("--check-first-run", "--check-setup-window")):
        return command
    catalogue = _catalogue(arguments)
    leave_update = any(flag in arguments for flag in _LEAVE_UPDATE)
    preferences = _last_flag(arguments, "--preferences-file")
    extra = []
    if "--data-dir" not in arguments and not any(argument.startswith("--data-dir=") for argument in arguments):
        if preferences is None:
            preferences = str(Path.cwd() / "offline.conf")
            if not leave_update:
                extra.extend(["--preferences-file", preferences])
        data_dir = str(Path(preferences).parent / "offline-data") if preferences else str(Path.cwd() / "offline-data")
        Path(data_dir).mkdir(parents=True, exist_ok=True)
        extra.extend(["--data-dir", data_dir])
    if preferences and not leave_update:
        ensure(preferences, catalogue)
    if not extra:
        return command
    return list(command) + extra


def _wrap_run(command, *args, **kwargs):
    return _RUN(apply(command), *args, **kwargs)


def _wrap_popen(command, *args, **kwargs):
    return _POPEN(apply(command), *args, **kwargs)


subprocess.run = _wrap_run
subprocess.Popen = _wrap_popen
