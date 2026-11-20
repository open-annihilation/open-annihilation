#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that a mod's data loads through its profile's layout, as a copied
install and as a mod folder over the base game folder.

--mod-install names a copied install of a mod: the base game's files with
the mod's copied over them. Its profile is the one among --profiles (one
oamod.yaml in each folder below it) whose revision archive the install
holds. The game starts a headless skirmish twice, on the dummy SDL drivers,
for --match-ticks ticks:

- on the copied install, with the profile given by --mod;
- on --game-dir with --mod-dir naming a scratch mod folder that holds only
  the files the install adds or changes, and the profile as its oamod.yaml.

Both runs must end with status 0, mount every archive they find, prepare a
match with the same number of unit types, and never look up a directory
the profile renames by its base game name (--trace-lookups).

Without a matching profile, or where the scratch folder cannot link to the
install's files, the check prints one line and exits with 77, which ctest
reports as skipped.
"""
import argparse
import filecmp
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# ctest's SKIP_RETURN_CODE.
SKIP = 77
RUN_TIMEOUT = 900
# The line a run prints once its match is prepared.
PREPARED = re.compile(r"Offline match world prepared for .* with (\d+) unit runtimes")
# The line discovery prints for an archive that does not mount.
SKIPPED_ARCHIVE = "skipping archive"
# The directories a profile may rename, as its layout names them.
RENAMEABLE = ("units", "weapons", "gamedata", "ai", "guis", "unitpics", "download")


class LayoutFailure(Exception):
    """A behaviour that differs from the expected one."""



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


def environment():
    """The environment of a headless run."""
    return dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                SDL_RENDER_DRIVER="software")


def resolved_profile(native, profile):
    """Resolves a profile with --print-profile; returns its effective form, or None."""
    result = subprocess.run(
        [*RUNNER, str(native), "--mod", str(profile), "--print-profile",
         "--accept-unimplemented-hacks"],
        env=environment(), timeout=RUN_TIMEOUT, check=False, capture_output=True, text=True,
        errors="replace")
    if result.returncode != 0:
        return None
    text = result.stdout
    end = text.rfind("}")
    return json.loads(text[:end + 1]) if end >= 0 else None


def entry_named(folder, name):
    """The entry of `folder` named `name` in any case, or None."""
    for entry in folder.iterdir():
        if entry.name.lower() == name.lower():
            return entry
    return None


def matching_profile(native, profiles, install):
    """The profile among `profiles` whose revision archive `install` holds."""
    for candidate in sorted(profiles.glob("*/oamod.yaml")):
        effective = resolved_profile(native, candidate)
        if not effective:
            continue
        revision = effective.get("layout", {}).get("revision-archive", "")
        if revision and entry_named(install, revision) is not None:
            return candidate, effective
    return None, None


def build_mod_folder(base, install, folder, profile):
    """Links into `folder` each file of `install` that `base` lacks or holds otherwise."""
    if folder.exists():
        shutil.rmtree(folder)
    for root, _, files in os.walk(install):
        relative = Path(root).relative_to(install)
        for name in files:
            source = Path(root) / name
            original = base / relative / name
            if original.is_file() and filecmp.cmp(original, source, shallow=False):
                continue
            target = folder / relative / name
            target.parent.mkdir(parents=True, exist_ok=True)
            os.link(source, target)
    shutil.copyfile(profile, folder / "oamod.yaml")


def run(native, workdir, name, arguments, ticks):
    """Starts a headless skirmish; returns its output and the paths it looked up."""
    lookups = workdir / f"{name}-lookups.txt"
    _offline_preferences(workdir / f"{name}.conf")
    result = subprocess.run(
        [*RUNNER, str(native), *arguments, "--accept-unimplemented-hacks", "--skip-intro", "--mute",
         "--headless-check", "--match-ticks", str(ticks),
         "--preferences-file", str(workdir / f"{name}.conf"), "--trace-lookups", str(lookups)],
        cwd=workdir, env=environment(), timeout=RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, end="")
        raise LayoutFailure(f"open-annihilation exited with {result.returncode} on the {name}")
    if SKIPPED_ARCHIVE in output:
        print(output, end="")
        raise LayoutFailure(f"an archive of the {name} did not mount")
    prepared = PREPARED.search(output)
    if not prepared:
        print(output, end="")
        raise LayoutFailure(f"the {name} prepared no match")
    return int(prepared.group(1)), lookups.read_text(errors="replace").splitlines()


def check_lookups(name, lookups, directories):
    """Fails when a lookup names a directory the profile renames by its base name."""
    renamed = [base for base in RENAMEABLE
               if directories.get(base, base).lower() != base.lower()]
    for line in lookups:
        first = line.replace("\\", "/").split("/", 1)[0].lower()
        if first in renamed:
            raise LayoutFailure(f"the {name} looked up {line} in the base directory {first}")
    print(f"mod layout check: the {name} looked up {len(lookups)} paths, none in "
          f"{', '.join(renamed) or 'a renamed directory'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--mod-install", type=Path, required=True)
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--match-ticks", type=int, default=30)
    arguments = parser.parse_args()
    # The runs start in the scratch folder.
    for name in ("native", "game_dir", "mod_install", "profiles", "scratch_root"):
        setattr(arguments, name, getattr(arguments, name).absolute())
    for folder in (arguments.game_dir, arguments.mod_install, arguments.profiles):
        if not folder.is_dir():
            print(f"mod layout check: {folder} is not a folder; skipped")
            return SKIP
    profile, effective = matching_profile(arguments.native, arguments.profiles,
                                          arguments.mod_install)
    if profile is None:
        print(f"mod layout check: no profile in {arguments.profiles} names a revision archive "
              f"{arguments.mod_install} holds; skipped")
        return SKIP
    workdir = arguments.scratch_root / "mod-layout"
    workdir.mkdir(parents=True, exist_ok=True)
    mod_folder = workdir / "mod-folder"
    try:
        build_mod_folder(arguments.game_dir, arguments.mod_install, mod_folder, profile)
    except OSError as error:
        print(f"mod layout check: the mod folder cannot link to the install's files ({error}); "
              "skipped")
        return SKIP
    directories = effective.get("layout", {}).get("directories", {})
    try:
        copied_types, copied = run(
            arguments.native, workdir, "copied install",
            ["--game-dir", str(arguments.mod_install), "--mod", str(profile)],
            arguments.match_ticks)
        check_lookups("copied install", copied, directories)
        layered_types, layered = run(
            arguments.native, workdir, "mod folder",
            ["--game-dir", str(arguments.game_dir), "--mod-dir", str(mod_folder)],
            arguments.match_ticks)
        check_lookups("mod folder", layered, directories)
        if copied_types != layered_types:
            raise LayoutFailure(f"the copied install prepared {copied_types} unit types and the "
                                f"mod folder {layered_types}")
    except LayoutFailure as failure:
        print(f"mod layout check: {failure}", file=sys.stderr)
        return 1
    print(f"mod layout check: {profile.parent.name} loads {copied_types} unit types as a copied "
          "install and as a mod folder")
    return 0


if __name__ == "__main__":
    sys.exit(main())
