#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that two machines play a network game by a mod's network rules.

--mod-install names a copied install of a mod; its profile is the one among
--profiles whose revision archive the install holds (as check_native_mod_layout
picks it). The game runs its in-process loopback check (--net-loopback-check)
headless on that install and profile: a host and a joiner over 127.0.0.1 go
from the battle room through a match to its end, by the profile's version
bytes, private channel, integrity check, votes, recorder and commander start
sync. The run must end with status 0 and both worlds' digests equal. When the
profile presents the recorder, each machine records the game into the mod's
folder in Recordings, in the player's own folder beside the preferences file,
and every recording the run wrote must then play back headless.

Without a matching profile the check prints one line and exits with 77,
which ctest reports as skipped.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import native_offline

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_native_mod_layout as layout  # noqa: E402

SUMMARY = re.compile(r"digests ([0-9a-f]{16}) / ([0-9a-f]{16})")
RECORDED = re.compile(r"multiplayer: recorded the game to (.+)$", re.MULTILINE)



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding='utf-8') if target.is_file() else 'open-annihilation-preferences 1\n'
    if 'open-annihilation.update-check' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.update-check" "never"\n'
    if 'open-annihilation.content-updates' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding='utf-8')


def run(native, arguments, log):
    """Runs the game headless; returns its status and output."""
    result = subprocess.run(
        [*layout.RUNNER, str(native), *arguments, "--accept-unimplemented-hacks", "--skip-intro",
         "--mute", "--headless-check"],
        env=layout.environment(), timeout=layout.RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")
    log.write_text(result.stdout + result.stderr)
    return result.returncode, result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--mod-install", type=Path, required=True)
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--ticks", type=int, default=300)
    arguments = parser.parse_args()
    for name in ("native", "mod_install", "profiles", "scratch_root"):
        setattr(arguments, name, getattr(arguments, name).absolute())
    for folder in (arguments.mod_install, arguments.profiles):
        if not folder.is_dir():
            print(f"mod loopback check: {folder} is not a folder; skipped")
            return layout.SKIP
    profile, effective = layout.matching_profile(arguments.native, arguments.profiles,
                                                 arguments.mod_install)
    if profile is None:
        print(f"mod loopback check: no profile in {arguments.profiles} names a revision archive "
              f"{arguments.mod_install} holds; skipped")
        return layout.SKIP
    workdir = arguments.scratch_root / "mod-loopback"
    workdir.mkdir(parents=True, exist_ok=True)
    _offline_preferences(workdir / "loopback.conf")
    common = ["--game-dir", str(arguments.mod_install), "--mod", str(profile)]
    status, output = run(
        arguments.native,
        [*common, "--net-loopback-check", str(arguments.ticks),
         "--preferences-file", str(workdir / "loopback.conf")],
        workdir / "loopback.txt")
    digests = SUMMARY.search(output)
    if status != 0 or digests is None or digests.group(1) != digests.group(2):
        print(f"mod loopback check: the loopback run ended with status {status}; "
              f"see {workdir / 'loopback.txt'}", file=sys.stderr)
        return 1
    print(f"mod loopback check: host and joiner agree by the profile's rules, "
          f"digest {digests.group(1)}")
    # The mod's folder in Recordings: its id, or "default (mod)" for the id
    # that names the folder of games without a mod.
    mod_id = effective.get("id", "")
    recordings = (workdir / "Open Annihilation" / "Recordings" /
                  ("default (mod)" if mod_id == "default" else mod_id))
    for index, recording in enumerate(RECORDED.findall(output)):
        if Path(recording.strip()).parent != recordings:
            print(f"mod loopback check: the recording {recording.strip()} is not in "
                  f"{recordings}", file=sys.stderr)
            return 1
        _offline_preferences(workdir / f"replay-{index}.conf")
        status, replay = run(
            arguments.native,
            [*common, "--play-demo", recording.strip(), "--match-ticks", str(arguments.ticks),
             "--preferences-file", str(workdir / f"replay-{index}.conf")],
            workdir / f"replay-{index}.txt")
        if status != 0:
            print(f"mod loopback check: the recording {recording.strip()} did not play back "
                  f"(status {status}); see {workdir / f'replay-{index}.txt'}", file=sys.stderr)
            return 1
        print(f"mod loopback check: the recording {Path(recording.strip()).name} in {recordings} "
              f"plays back")
    return 0


if __name__ == "__main__":
    sys.exit(main())
