#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that every unit that stockpiles works its weapon page by a mod's profile.

--mod-install names a copied install of a mod; its profile is the one among
--profiles whose revision archive the install holds (as check_native_mod_layout
picks it). The game runs its stockpile build check (--check-stockpile-builds)
on dummy SDL devices on that install and profile: every unit type whose first
weapon stockpiles opens its weapon page when selected, queues and drops
rounds, and the cheapest round is built and kept through a save and a load.

Without a matching profile the check prints one line and exits with 77,
which ctest reports as skipped.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import native_offline

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_mod_layout as layout  # noqa: E402



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding="utf-8") if target.is_file() else "open-annihilation-preferences 1\n"
    if "open-annihilation.update-check" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.update-check" "never"\n'
    if "open-annihilation.content-updates" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--mod-install", type=Path, required=True)
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    arguments = parser.parse_args()
    for name in ("native", "mod_install", "profiles", "scratch_root"):
        setattr(arguments, name, getattr(arguments, name).absolute())
    for folder in (arguments.mod_install, arguments.profiles):
        if not folder.is_dir():
            print(f"mod stockpile check: {folder} is not a folder; skipped")
            return layout.SKIP
    profile, _ = layout.matching_profile(arguments.native, arguments.profiles,
                                         arguments.mod_install)
    if profile is None:
        print(f"mod stockpile check: no profile in {arguments.profiles} names a revision archive "
              f"{arguments.mod_install} holds; skipped")
        return layout.SKIP
    workdir = arguments.scratch_root / "mod-stockpile"
    workdir.mkdir(parents=True, exist_ok=True)
    _offline_preferences(workdir / "stockpile.conf")
    result = subprocess.run(
        [*layout.RUNNER, str(arguments.native), "--game-dir", str(arguments.mod_install),
         "--mod", str(profile), "--accept-unimplemented-hacks", "--skip-intro", "--mute",
         "--check-stockpile-builds", "--preferences-file", str(workdir / "stockpile.conf")],
        cwd=workdir, env=layout.environment(), timeout=layout.RUN_TIMEOUT, check=False,
        capture_output=True, text=True, errors="replace")
    log = workdir / "stockpile.txt"
    log.write_text(result.stdout + result.stderr)
    summary = [line for line in result.stdout.splitlines()
               if line.startswith("stockpile build check:")]
    if result.returncode != 0 or not summary:
        print(f"mod stockpile check: the run ended with status {result.returncode}; see {log}",
              file=sys.stderr)
        return 1
    print(f"mod {summary[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
