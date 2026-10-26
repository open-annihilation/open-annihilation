#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Run oa-update on a plain folder and check the swap, a bad package and a failed rename.

The program under test starts this script with --started. That path writes
the arguments it was given and exits. The rest makes a throwaway key, a
feed and a release zip, then runs oa-update. Nothing is fetched.
"""

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path


PASSPHRASE = "correct horse battery\n"
OLD = {
    "one.txt": b"old-one\n",
    "two.txt": b"old-two\n",
    "three.txt": b"old-three\n",
}
NEW = {
    "one.txt": b"new-one\n",
    "two.txt": b"new-two\n",
    "three.txt": b"new-three\n",
}
TOP = "open-annihilation-0.0.2-linux-x86_64"
ZIP_NAME = TOP + ".zip"


def fail(message):
    print("check_update_helper: " + message, file=sys.stderr)
    return 1


def spell(path):
    """An absolute path with forward slashes, as the plan stores one."""
    return Path(path).resolve().as_posix()


def run(command, env=None):
    completed = subprocess.run(command, capture_output=True, text=True, env=env)
    if completed.returncode != 0:
        sys.stderr.write(completed.stdout)
        sys.stderr.write(completed.stderr)
    return completed


def write_tree(folder, files):
    folder.mkdir(parents=True, exist_ok=True)
    for name, body in files.items():
        (folder / name).write_bytes(body)


def make_zip(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for name, body in NEW.items():
            archive.writestr(TOP + "/" + name, body)


def public_key(text):
    for line in text.splitlines():
        if line.startswith("public: "):
            return line[len("public: ") :]
    return ""


def feed_written(text):
    """The feed path `engine-feed write` printed on its `wrote` line."""
    prefix = "wrote "
    marker = " release "
    for line in text.splitlines():
        if not line.startswith(prefix) or marker not in line:
            continue
        path = line[len(prefix) : line.rfind(marker)]
        if path:
            return path
    return ""


def write_plan(path, install, updates, package, preferences, program, script, marker):
    plan = {
        "plan": 1,
        "action": "install",
        "route": "check-folder",
        "wait_for": 0,
        "install": spell(install),
        "program": spell(program),
        "arguments": [spell(script), "--started", spell(marker)],
        "from_version": "0.0.1",
        "feed": spell(path.parent / "feed.json"),
        "signature": spell(path.parent / "feed.json.sig"),
        "release": 2,
        "package": "linux-x86_64",
        "file": spell(package),
        "installer": False,
        "preferences": spell(preferences),
        "updates": spell(updates),
        "title": "Installing 0.0.2",
        "line": "0.0.1 is kept, in case you want it back.",
    }
    path.write_text(json.dumps(plan) + "\n", encoding="utf-8")


def marker_lines(marker):
    if not marker.is_file():
        return []
    return marker.read_text(encoding="utf-8").splitlines()


def await_marker(marker):
    """The arguments the started program wrote.

    The helper returns as soon as that program has been started, so the
    marker can appear a moment later. Two matching reads mean the write
    has finished.
    """
    deadline = time.monotonic() + 5
    previous = None
    while True:
        lines = marker_lines(marker)
        if lines and lines == previous:
            return lines
        previous = lines
        if time.monotonic() >= deadline:
            return lines
        time.sleep(0.05)


def same_files(folder, files):
    for name, body in files.items():
        path = folder / name
        if not path.is_file() or path.read_bytes() != body:
            return False
    return True


def helper_env():
    env = os.environ.copy()
    env["SDL_VIDEO_DRIVER"] = "dummy"
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    return env


def run_helper(oa_update, plan, feed_key):
    return run(
        [oa_update, "--plan", str(plan), "--feed-key", feed_key],
        env=helper_env(),
    )


def prepare_case(root, package, preferences, program, script):
    install = root / "install"
    updates = root / "updates"
    marker = root / "marker.txt"
    plan = root / "plan.json"
    write_tree(install, OLD)
    updates.mkdir()
    write_plan(plan, install, updates, package, preferences, program, script, marker)
    # The plan's feed paths were written beside the plan. Point them at the shared feed.
    return install, updates, marker, plan


def retarget_feed(plan_path, feed, signature):
    plan = json.loads(plan_path.read_text(encoding="utf-8"))
    plan["feed"] = spell(feed)
    plan["signature"] = spell(signature)
    plan_path.write_text(json.dumps(plan) + "\n", encoding="utf-8")


def check_swapped(install, updates, preferences, marker):
    if not same_files(install, NEW):
        return fail("the install does not hold the new files")
    previous = updates / "previous"
    if not same_files(previous, OLD):
        return fail("the kept folder does not hold the old files")
    copied = previous / "preferences.conf"
    if not copied.is_file() or copied.read_bytes() != preferences.read_bytes():
        return fail("the preferences copy does not match")
    state = json.loads((updates / "state.json").read_text(encoding="utf-8"))
    if state.get("installed_version") != "0.0.2" or state.get("installed_release") != 2:
        return fail("state.json does not name the installed release")
    if state.get("kept_version") != "0.0.1" or state.get("confirmed") is not False:
        return fail("state.json does not name the kept version")
    if state.get("failed_starts") != 0:
        return fail("state.json does not start the failure count at 0")
    lines = await_marker(marker)
    if lines[-2:] != ["--updated-from", "0.0.1"]:
        return fail("the started arguments do not end in --updated-from 0.0.1: " + repr(lines))
    if (updates / "journal.json").exists() or (updates / "2" / "stage").exists():
        return fail("the journal or the staging folder was left behind")
    return 0


def check_unchanged(install, marker, updated_from):
    if not same_files(install, OLD):
        return fail("the install changed")
    lines = await_marker(marker)
    has_flag = "--updated-from" in lines
    if has_flag != updated_from:
        return fail("the started arguments were " + repr(lines))
    if not lines:
        return fail("the old version was not started")
    return 0


def main(argv):
    if "--started" in argv:
        at = argv.index("--started")
        if at + 1 >= len(argv):
            return fail("--started needs a marker")
        Path(argv[at + 1]).write_text("\n".join(argv) + "\n", encoding="utf-8")
        return 0
    if len(argv) != 3:
        return fail("usage: check_update_helper.py OA-UPDATE OA-TOOL")
    oa_update = argv[1]
    oa_tool = argv[2]
    script = Path(__file__).resolve()
    scratch = Path(tempfile.mkdtemp(prefix="oa-v07-run-"))
    try:
        passphrase = scratch / "passphrase"
        passphrase.write_text(PASSPHRASE, encoding="utf-8")
        key = scratch / "key"
        made = run(
            [
                oa_tool,
                "catalogue",
                "keygen",
                "--id",
                "v07-test",
                "--out",
                str(key),
                "--passphrase-file",
                str(passphrase),
            ]
        )
        if made.returncode != 0:
            return fail("keygen failed")
        public = public_key(made.stdout)
        if not public.startswith("ed25519:"):
            return fail("keygen did not print a public key")
        feed_key = "v07-test=" + public

        package = scratch / "packages" / ZIP_NAME
        make_zip(package)
        # Attached, so oa-tool does not treat a bare `--version` as its own
        # version line and leave without writing the feed.
        feed_asked = scratch / "feed.json"
        written = run(
            [
                oa_tool,
                "engine-feed",
                "write",
                "--version=0.0.2",
                "--date",
                "2026-10-24",
                "--from",
                "0.0.1",
                "--page",
                "https://example.net/v0.0.2",
                "--mirror",
                "http://downloads.example.net/v1/engine/0.0.2/",
                "--out",
                str(feed_asked),
                str(package),
            ]
        )
        if written.returncode != 0:
            return fail("engine-feed write failed")
        wrote = feed_written(written.stdout)
        if not wrote:
            sys.stderr.write(written.stdout)
            return fail("engine-feed write did not name the feed")
        feed = Path(wrote)
        if not feed.is_file():
            return fail("engine-feed write named " + wrote + ", which is not a file")
        signed = run(
            [
                oa_tool,
                "engine-feed",
                "sign",
                "--key",
                str(key),
                "--passphrase-file",
                str(passphrase),
                str(feed),
            ]
        )
        if signed.returncode != 0:
            return fail("engine-feed sign failed")
        signature = Path(str(feed) + ".sig")
        if not signature.is_file():
            return fail("the signature was not written")

        preferences = scratch / "preferences.conf"
        preferences.write_bytes(b"volume=1\n")

        swapped = scratch / "swapped"
        install, updates, marker, plan = prepare_case(
            swapped, package, preferences, sys.executable, script
        )
        retarget_feed(plan, feed, signature)
        first = run_helper(oa_update, plan, feed_key)
        if first.returncode != 0:
            sys.stderr.write(first.stdout)
            sys.stderr.write(first.stderr)
            return fail("the swap exited " + str(first.returncode))
        status = check_swapped(install, updates, preferences, marker)
        if status != 0:
            return status

        longer = scratch / "longer" / ZIP_NAME
        longer.parent.mkdir()
        longer.write_bytes(package.read_bytes() + b"\n")
        refused = scratch / "refused"
        install, updates, marker, plan = prepare_case(
            refused, longer, preferences, sys.executable, script
        )
        retarget_feed(plan, feed, signature)
        second = run_helper(oa_update, plan, feed_key)
        if second.returncode != 1:
            sys.stderr.write(second.stdout)
            sys.stderr.write(second.stderr)
            return fail("a long package exited " + str(second.returncode))
        status = check_unchanged(install, marker, False)
        if status != 0:
            return status

        if os.name == "nt":
            # Renaming out of a read-only directory is a POSIX check.
            return 0
        if os.geteuid() == 0:
            # Root ignores the directory's mode, so the rename cannot be
            # blocked this way (the Linux builds run their tests as root).
            return 0
        blocked = scratch / "blocked"
        install, updates, marker, plan = prepare_case(
            blocked, package, preferences, sys.executable, script
        )
        retarget_feed(plan, feed, signature)
        # A file mode of 0444 does not stop rename on macOS. The directory
        # is set to 0555, which refuses the rename, and one file to 0444.
        locked = install / "one.txt"
        locked.chmod(0o444)
        install.chmod(0o555)
        try:
            third = run_helper(oa_update, plan, feed_key)
        finally:
            install.chmod(0o755)
            locked.chmod(0o644)
        if third.returncode != 3:
            sys.stderr.write(third.stdout)
            sys.stderr.write(third.stderr)
            return fail("a blocked rename exited " + str(third.returncode))
        return check_unchanged(install, marker, False)
    finally:
        # The read-only case restores its own mode before this cleanup.
        for folder in scratch.rglob("*"):
            if folder.is_dir():
                folder.chmod(folder.stat().st_mode | stat.S_IWUSR)
        scratch.chmod(scratch.stat().st_mode | stat.S_IWUSR)
        shutil.rmtree(scratch)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
