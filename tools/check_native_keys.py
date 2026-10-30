#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check --keys, --keys-reset and the one-time import of stored keys.

A folder layout that binds order.attack to A, played through the key table
with --trace-input, passes and prints the action. An unknown id exits 2 and
names itself. A preferences file that names a layout that is not installed,
with --keys-reset, plays and writes no own layout. A profile that turns on
ui.build-tools, with Eye KeyCode 113, leaves own-oa-default holding
build.line on @KeyQ and lists the mod in open-annihilation.keys.imported; a
second run changes neither file. A .oakeys file with the same binding plays
and leaves its Keys folder empty.
"""

import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import zipfile

# A command that starts the executable under test; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
RUN_TIMEOUT = 600
PREFERENCES_HEADER = "open-annihilation-preferences 1"
USER_FOLDER = "Open Annihilation"
ATTACK_LINE = "input action pressed order.attack chord=A context=play"
KEY_TABLE_LINE = "rows match"
MISSING_LINE = "open-annihilation: keys: --keys: no-such-layout: it is not installed"
MATCH_TICK = "tick    30:"
IMPORT_LINE = "open-annihilation: keys: imported build.line from KeyCode as @KeyQ"
MOD_ID = "ridge-build-keys"
LAYOUT_TEXT = """oakeys: 1
id: ridge-attack
name: "Ridge attack"
version: "1.0"
author: {name: "Ada"}
tags: [grid]
base: oa-default
bindings:
  order.attack: [A]
"""
PROFILE_TEXT = """oamod: 1
id: ridge-build-keys
name: Ridge build keys
version: "1.0"
requires: {base: ta-3.1c, catalogue: 1}
author: {name: unknown}
packaging: {revision: 1, date: 2026-10-26, packager: Open Annihilation}
hacks:
  ui.build-tools: true
"""


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def run(native, game_dir, work, args):
    """Runs one start. Returns the status and the combined output."""
    environment = dict(os.environ)
    environment["SDL_VIDEO_DRIVER"] = "dummy"
    environment["SDL_AUDIO_DRIVER"] = "dummy"
    environment["SDL_RENDER_DRIVER"] = "software"
    command = RUNNER + [
        native,
        "--skip-intro",
        "--mute",
        "--game-dir",
        game_dir,
        *args,
    ]
    completed = subprocess.run(
        command,
        cwd=work,
        env=environment,
        capture_output=True,
        text=True,
        timeout=RUN_TIMEOUT,
        check=False,
    )
    output = completed.stdout + completed.stderr
    return completed.returncode, output


def show(name, output):
    """Prints the tail of a failed run."""
    tail = output[-4000:]
    sys.stderr.write(f"--- {name} ---\n{tail}\n")


def write(path, text):
    """Writes a text file, making its folder."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def preferences(lines):
    """A preferences file of the quoted lines."""
    body = PREFERENCES_HEADER + "\n"
    for key, value in lines:
        body += f'"{key}" "{value}"\n'
    return body


def write_oakeys(path, text):
    """Writes a .oakeys whose manifest is deflated, as the packer stores it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        info = zipfile.ZipInfo("oakeys.yaml")
        info.compress_type = zipfile.ZIP_DEFLATED
        info.date_time = (1980, 1, 1, 0, 0, 0)
        archive.writestr(info, text)


def keys_unused(folder):
    """True when Keys is absent or holds nothing."""
    keys = folder / "Keys"
    if not keys.exists():
        return True
    return not any(keys.iterdir())


def main():
    """Runs the five checks."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--key-table", required=True)
    parser.add_argument("--scratch-root", required=True)
    parsed = parser.parse_args()
    work = Path(parsed.scratch_root) / "native-keys"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    layout = work / "ridge-attack"
    write(layout / "oakeys.yaml", LAYOUT_TEXT)
    # --headless-check returns from the menu before --check-key-table runs.
    # This start is the one native-key-table uses, plus the layout and the trace.
    status, output = run(
        parsed.native,
        parsed.game_dir,
        work,
        [
            "--keys",
            str(layout),
            "--check-key-table",
            parsed.key_table,
            "--trace-input",
            "--preferences-file",
            str(work / "attack.conf"),
        ],
    )
    if status != 0 or ATTACK_LINE not in output or KEY_TABLE_LINE not in output:
        show("attack", output)
        raise ScriptFailure(f"--keys layout exited {status}")

    # The unknown id is refused while the game is built, before the menu check
    # this flag would otherwise run. The line below is that refusal.
    status, output = run(
        parsed.native,
        parsed.game_dir,
        work,
        [
            "--headless-check",
            "--keys",
            "no-such-layout",
            "--preferences-file",
            str(work / "missing.conf"),
        ],
    )
    if status != 2 or MISSING_LINE not in output or "native check:" in output:
        show("missing", output)
        raise ScriptFailure(f"--keys no-such-layout exited {status}")

    reset_prefs = work / "reset.conf"
    write(
        reset_prefs,
        preferences([("open-annihilation.keys.layout", "no-such-layout")]),
    )
    before = reset_prefs.read_bytes()
    # --match-ticks plays the skirmish. Without it the menu check returns
    # and never prints a tick line.
    status, output = run(
        parsed.native,
        parsed.game_dir,
        work,
        [
            "--headless-check",
            "--match-ticks",
            "30",
            "--keys-reset",
            "--preferences-file",
            str(reset_prefs),
        ],
    )
    own = work / USER_FOLDER / "Keys" / "own-oa-default" / "oakeys.yaml"
    if status != 0 or MATCH_TICK not in output or own.exists():
        show("reset", output)
        raise ScriptFailure(f"--keys-reset exited {status}")
    if b"open-annihilation.keys.imported" in reset_prefs.read_bytes():
        raise ScriptFailure("--keys-reset wrote the imported list")
    if b"no-such-layout" not in reset_prefs.read_bytes():
        raise ScriptFailure("--keys-reset dropped the layout preference")
    del before

    profile = work / "ridge-build-keys.oamod.yaml"
    write(profile, PROFILE_TEXT)
    imported_prefs = work / "import.conf"
    write(imported_prefs, preferences([("Eye|KeyCode", "113")]))
    import_args = [
        "--headless-check",
        "--match-ticks",
        "30",
        "--mod",
        str(profile),
        "--preferences-file",
        str(imported_prefs),
    ]
    status, output = run(parsed.native, parsed.game_dir, work, import_args)
    own_text = own.read_text(encoding="utf-8") if own.is_file() else ""
    imported_text = imported_prefs.read_text(encoding="utf-8")
    if (
        status != 0
        or MATCH_TICK not in output
        or IMPORT_LINE not in output
        or 'build.line: ["@KeyQ"]' not in own_text
        or "open-annihilation.keys.imported" not in imported_text
        or MOD_ID not in imported_text
    ):
        show("import", output)
        raise ScriptFailure(f"import exited {status}")
    own_bytes = own.read_bytes()
    prefs_bytes = imported_prefs.read_bytes()
    status, output = run(parsed.native, parsed.game_dir, work, import_args)
    if (
        status != 0
        or MATCH_TICK not in output
        or IMPORT_LINE in output
        or own.read_bytes() != own_bytes
        or imported_prefs.read_bytes() != prefs_bytes
    ):
        show("import again", output)
        raise ScriptFailure(f"a second run exited {status} or changed a file")

    # A .oakeys is read in memory. Nothing is installed, and the layout
    # preference is not written. The user folder is not the import case's.
    pack = work / "ridge-attack.oakeys"
    write_oakeys(pack, LAYOUT_TEXT)
    pack_user = work / "pack-user"
    pack_prefs = work / "pack.conf"
    status, output = run(
        parsed.native,
        parsed.game_dir,
        work,
        [
            "--keys",
            str(pack),
            "--check-key-table",
            parsed.key_table,
            "--trace-input",
            "--preferences-file",
            str(pack_prefs),
            "--user-folder",
            str(pack_user),
        ],
    )
    prefs_text = pack_prefs.read_text(encoding="utf-8") if pack_prefs.is_file() else ""
    if (
        status != 0
        or ATTACK_LINE not in output
        or KEY_TABLE_LINE not in output
        or not keys_unused(pack_user)
        or "open-annihilation.keys.layout" in prefs_text
    ):
        show("oakeys", output)
        raise ScriptFailure(f"--keys .oakeys exited {status}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ScriptFailure, subprocess.TimeoutExpired, OSError) as failure:
        sys.stderr.write(f"{failure}\n")
        sys.exit(1)
