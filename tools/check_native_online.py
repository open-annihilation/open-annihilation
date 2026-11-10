#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Fail when a native check's preferences leave an online check at automatically.

A script that starts the game imports tools/native_offline.py. That module
writes the offline preferences when the file is absent, makes the private
data folder, and adds the flags. A direct oa-game test receives the same
from cmake/OaOfflineGameTests.cmake. The lines that registration writes
are kept in a seed beside the preferences file. A fixture may remove the
file the game reads, and the game may rewrite it; this check reads the seed.

A check that is about updates or the catalogue may set a check to
automatically only when the same file points the launch at a loopback feed.
"""

import argparse
import ast
import json
import posixpath
import re
import subprocess
import sys
from pathlib import Path

UPDATE = "open-annihilation.update-check"
CONTENT = "open-annihilation.content-updates"
AUTOMATIC = re.compile(
    r'open-annihilation\.(?:update-check|content-updates)"\s*"automatically"'
)
LOOPBACK = ("127.0.0.1", "--update-feed")
# The update pill keeps the update check at automatically and passes
# --update-feed from a local publisher. That launch never asks Core Prime.
ALLOW_AUTOMATIC = {
    "check_native_boneyards_menu_keys.py":
        "the update pill keeps the update check at automatically with --update-feed from a local publisher",
    "check_native_update_download.py":
        "the download check leaves the update check at its default and serves the feed on loopback "
        "with --update-feed; its catalogue check stays never",
}
GAME_EXECUTABLES = frozenset({"open-annihilation", "open-annihilation.exe"})
MAX_LISTING_BYTES = 64 << 20


def files(root):
    """The native checks and the server that starts the game for one of them."""
    found = []
    for path in root.rglob("check_native_*.py"):
        if any(part in path.parts for part in ("build", ".git")):
            continue
        found.append(path)
    found.append(root / "tools/mcp/oa_mcp_server.py")
    return sorted(set(found))


def starts_game(text):
    """True when this file starts the game, not when it only names a preference.

    The preference key names appear in files that never launch. A launch
    passes a game flag or goes through automation.Game, and starts a process.
    """
    launches = (
        "subprocess.run" in text
        or "subprocess.Popen" in text
        or "automation.Game" in text
    )
    game = (
        "--game-dir" in text
        or "--fark" in text
        or "--headless-check" in text
        or "automation.Game" in text
    )
    return launches and game


def problems(path, root):
    """Sentences naming how this file leaves an online check at automatically."""
    text = path.read_text(encoding="utf-8")
    name = path.name
    found = []
    loopback = any(mark in text for mark in LOOPBACK)
    if AUTOMATIC.search(text) and not loopback and name not in ALLOW_AUTOMATIC:
        found.append(f"{path.relative_to(root)}: an online check is set to automatically")
    if not starts_game(text):
        return found
    if not imports_native_offline(text):
        found.append(
            f"{path.relative_to(root)}: starts the game without importing native_offline"
        )
    return found


def imports_native_offline(text):
    """True when the file imports native_offline at its top level.

    A comment, a string or an import inside a function that nothing calls
    does not put the helper in front of a launch.
    """
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return False
    for node in tree.body:
        if isinstance(node, ast.Import):
            if any(alias.name.split(".")[0] == "native_offline" for alias in node.names):
                return True
        elif isinstance(node, ast.ImportFrom):
            if node.level == 0 and (node.module or "").split(".")[0] == "native_offline":
                return True
    return False


def game_launch(command):
    """True when the test's own command is the game, not a script that starts it."""
    if not command:
        return False
    base = posixpath.basename(str(command[0]).replace("\\", "/"))
    return base in GAME_EXECUTABLES


def flag_value(command, flag):
    """The argument after the last flag. A later flag replaces an earlier one."""
    value = None
    for index, argument in enumerate(command):
        if argument == flag and index + 1 < len(command):
            value = command[index + 1]
    return value


def preferences_file_offline(path, allow_update=False, allow_when_asked=False, exact=False):
    """True when the preferences file turns both online checks off.

    exact reads the named file itself, as for the start file a fixture copies
    over the live one.

    allow_update is for a check that serves its own release feed, where the
    update check stays on and only the catalogue check is off.
    allow_when_asked is the Boneyards mod entry: never on the update check
    asks to turn the online features on, so that check is when-asked, which
    does not look for a release until the player asks.
    """
    # The seed is the file registration wrote. The game rewrites the live
    # file, so a check after a run must not read that.
    file = Path(path) if exact else Path(str(path) + ".offline-seed")
    if not file.is_file():
        file = Path(path)
    if not file.is_file():
        return False
    text = file.read_text(encoding="utf-8", errors="replace")
    update_off = f'"{UPDATE}" "never"' in text
    if allow_update:
        update_off = True
    if allow_when_asked:
        update_off = update_off or f'"{UPDATE}" "when-asked"' in text
    content_off = f'"{CONTENT}" "never"' in text or f'"{CONTENT}" "library"' in text
    return update_off and content_off


def command_values(command):
    """The -DNAME=value arguments of a cmake command, as a dictionary."""
    return dict(
        str(a)[2:].split("=", 1) for a in command if str(a).startswith("-D") and "=" in str(a)
    )


def fixture_resets(tests):
    """What the registered fixtures remove, and what each puts back.

    Returns (removed, restored): the paths a fixture removes before the tests
    that use them, and for each file a fixture writes, the file it copies from.
    A fixture that removes a preferences file and puts nothing back leaves the
    game on its defaults: the lines written when CMake configured are gone.
    cmake/OaOfflineAddTest.cmake registers a fixture that removed a .conf as a
    script that copies the seed back (-DREMOVALS and -DRESTORES). The language
    notice checks copy a start file over the live one (-Ddir, -Dlive, -Dstart),
    so the game starts from the start file.
    """
    removed = set()
    restored = {}
    for test in tests:
        command = test.get("command") or []
        if not isinstance(command, list) or len(command) < 2:
            continue
        if posixpath.basename(str(command[0])) in ("cmake", "cmake.exe") and command[1:3] == ["-E", "rm"]:
            removed.update(str(a) for a in command[3:] if not str(a).startswith("-"))
        elif "-P" in command:
            values = command_values(command)
            removed.update(path for path in values.get("REMOVALS", "").split(";") if path)
            for pair in values.get("RESTORES", "").split(";"):
                destination, bar, source = pair.partition("|")
                if bar and destination and source:
                    restored[destination] = source
            if {"dir", "live", "start"} <= values.keys():
                restored[posixpath.join(values["dir"], values["live"])] = posixpath.join(
                    values["dir"], values["start"]
                )
    return removed, restored


def is_removed(path, removed):
    """True when a fixture removes this file, or a folder that holds it."""
    return any(path == gone or path.startswith(gone.rstrip("/") + "/") for gone in removed)


def registered_problems(tests):
    """A direct game test with no offline preferences, or with the shared data folder."""
    found = []
    removed, restored = fixture_resets(tests)
    for test in tests:
        command = test.get("command") or []
        if not isinstance(command, list) or not game_launch(command):
            continue
        name = test.get("name", "")
        if "--data-dir" not in command:
            found.append(f"{name}: the game starts on the shared data folder")
            continue
        # The welcome setup starts from no preferences and serves its catalogue
        # on this machine. The release checks serve their own feed too.
        if any(
            flag in command
            for flag in ("--check-first-run", "--check-setup-window")
        ):
            continue
        prefs = flag_value(command, "--preferences-file")
        if prefs is not None and is_removed(prefs, removed) and prefs not in restored:
            found.append(
                f"{name}: a fixture removes its preferences file and puts nothing back, "
                "so the offline lines never reach the launch"
            )
            continue
        local_feed = "--check-engine-feed" in command or "--check-update-offer" in command
        mod_entry = "--check-boneyards-mod-entry" in command
        # A fixture that copies a file over the live one decides what the game reads.
        source = restored.get(prefs) if prefs is not None else None
        if prefs is None or not preferences_file_offline(
            source or prefs, allow_update=local_feed, allow_when_asked=mod_entry, exact=bool(source)
        ):
            found.append(f"{name}: preferences leave an online check at automatically")
    return found


def list_tests(ctest, build, config):
    """The tests a build registers, as ctest --show-only=json-v1 lists them."""
    command = [ctest, "--test-dir", build, "--show-only=json-v1"]
    if config:
        command += ["-C", config]
    result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
    if result.returncode != 0 or len(result.stdout) > MAX_LISTING_BYTES:
        raise RuntimeError(f"{' '.join(command)} failed: {result.stderr.strip()}")
    listing = json.loads(result.stdout)
    tests = listing.get("tests") if isinstance(listing, dict) else None
    if not isinstance(tests, list):
        raise RuntimeError("the test listing holds no list of tests")
    return tests


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path)
    parser.add_argument("root_positional", nargs="?", type=Path)
    parser.add_argument("--ctest")
    parser.add_argument("--build")
    parser.add_argument("--config", default="")
    args = parser.parse_args(argv[1:])
    root = (args.root or args.root_positional or Path(__file__).resolve().parents[1]).resolve()
    found = []
    checked = files(root)
    for path in checked:
        if path.name == "check_native_online.py":
            continue
        found.extend(problems(path, root))
    registered = 0
    if args.ctest and args.build:
        tests = list_tests(args.ctest, args.build, args.config)
        registered = sum(1 for test in tests if game_launch(test.get("command") or []))
        if registered == 0:
            found.append("no registered test starts the game: is the build complete?")
        found.extend(registered_problems(tests))
    if found:
        print("\n".join(found), file=sys.stderr)
        return 1
    print(
        "native checks: online checks are never left at automatically "
        f"({len(checked)} files, {registered} game tests)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
