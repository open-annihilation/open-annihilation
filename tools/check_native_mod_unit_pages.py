#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a mod's unit pages fit the side column by its profile.

--mod-install names a copied install of a mod; its profile is the one among
--profiles whose revision archive the install holds (as check_native_mod_layout
picks it). The game runs its unit pages check (--check-unit-pages scaled) on
dummy SDL devices on that install and profile for a commander, a lab, a unit
that builds nothing and one that stockpiles: on windows of 640x480, 1280x720,
1920x1080 and 2560x1440 the whole side column must be drawn at the one scale
at which the profile's tallest unit page ends on the window's last row, as
wide as its 128 columns at that scale, with the bars and the battlefield from
its edge on; each of their pages is opened and must be drawn whole as its file
places it, at the column's scale, and each control the side column draws, the
radar and the battlefield's edge must take a click where they are drawn.

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

# The unit types checked: a commander, a lab, a unit that builds nothing and
# one that stockpiles.
UNIT_TYPES = ("ARMCOM", "ARMLAB", "ARMMARK", "ARMAMD")



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
            print(f"mod unit pages check: {folder} is not a folder; skipped")
            return layout.SKIP
    profile, _ = layout.matching_profile(arguments.native, arguments.profiles,
                                         arguments.mod_install)
    if profile is None:
        print(f"mod unit pages check: no profile in {arguments.profiles} names a revision "
              f"archive {arguments.mod_install} holds; skipped")
        return layout.SKIP
    workdir = arguments.scratch_root / "mod-unit-pages"
    workdir.mkdir(parents=True, exist_ok=True)
    _offline_preferences(workdir / "unit-pages.conf")
    result = subprocess.run(
        [*layout.RUNNER, str(arguments.native), "--game-dir", str(arguments.mod_install),
         "--mod", str(profile), "--accept-unimplemented-hacks", "--skip-intro", "--mute",
         "--check-unit-pages", "scaled:" + ",".join(UNIT_TYPES), "--preferences-file", str(workdir / "unit-pages.conf")],
        cwd=workdir, env=layout.environment(), timeout=layout.RUN_TIMEOUT, check=False,
        capture_output=True, text=True, errors="replace")
    log = workdir / "unit-pages.txt"
    log.write_text(result.stdout + result.stderr)
    summary = [line for line in result.stdout.splitlines()
               if line.startswith("unit pages check:")]
    if result.returncode != 0 or not summary:
        print(f"mod unit pages check: the run ended with status {result.returncode}; see {log}",
              file=sys.stderr)
        for line in result.stderr.splitlines():
            if line.startswith("unit pages check:"):
                print(line, file=sys.stderr)
        return 1
    print(f"mod {summary[-1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
