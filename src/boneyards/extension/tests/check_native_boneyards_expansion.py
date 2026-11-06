#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Opens the Boneyards from the installed expansion, not the game folder.

The bundle's files are linked (or copied, when a link will not take) from
the installed game into a scratch user folder's Expansions/totala/boneyards/.
The game folder is the same install with the bundle's root file and its
Boneyards folder left out. Each run deletes that scratch folder first, so a
second run starts from the same files. Nothing is written outside it.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

WORK_MARK = "X02-boneyards-expansion"
# --boneyards-login does not print the entry line the no-login check prints.
# The overlay has drawn once the help console is up, and the page is the
# loose help file, which this game folder does not hold.
HELP_SHOWN = "boneyards login check: the help console showed"
CONTENTS = re.compile(
    r'boneyards login check: contents console: ([1-9][0-9]*) rows, '
    r'page ".*" shown with .* light dark'
)
COPY_NAME = re.compile(r'ContentFile\{"([^"]+)"')


def copy_names(table_path):
    """The bundle's copy names, in the table's order."""
    with open(table_path, encoding="utf-8") as handle:
        text = handle.read()
    start = text.find("constexpr ContentFile k_files_boneyards[]")
    if start < 0:
        raise SystemExit("the bundle table has no boneyards file list")
    end = text.find("};", start)
    names = COPY_NAME.findall(text[start:end])
    if not names or names[0] != "by.ccx":
        raise SystemExit("the bundle table's file list does not start with by.ccx")
    return names


def find_source(root, copy_name):
    """The installed file for one copy name, matching ASCII case if needed."""
    direct = os.path.join(root, *copy_name.split("/"))
    if os.path.isfile(direct):
        return direct
    current = root
    for part in copy_name.split("/"):
        if not os.path.isdir(current):
            return None
        match = None
        for name in os.listdir(current):
            if name.casefold() == part.casefold():
                match = os.path.join(current, name)
                break
        if match is None:
            return None
        current = match
    return current if os.path.isfile(current) else None


def place_file(src, dst):
    """Puts src at dst as a hard link, or a copy when the link will not take.

    A symbolic link is never left in the scratch tree.
    """
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst):
        os.remove(dst)
    try:
        os.link(src, dst, follow_symlinks=True)
    except OSError:
        shutil.copyfile(src, dst)


def reset_work(work):
    """Deletes the scratch folder and makes it again."""
    work = os.path.abspath(work)
    if WORK_MARK not in os.path.basename(work):
        raise SystemExit("refusing to reset a folder that is not the X02 scratch: %s" % work)
    if os.path.lexists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    return work


def link_tree(src, dst, skip_root):
    """Links every file under src into dst, leaving the named root names out."""
    src = os.path.abspath(src)
    seen = set()

    def walk(current, rel):
        real = os.path.realpath(current)
        if real in seen:
            return
        seen.add(real)
        os.makedirs(dst if rel == "" else os.path.join(dst, rel), exist_ok=True)
        with os.scandir(current) as scan:
            for entry in scan:
                if rel == "" and entry.name.casefold() in skip_root:
                    continue
                child = entry.name if rel == "" else rel + "/" + entry.name
                if entry.is_dir(follow_symlinks=True):
                    walk(entry.path, child)
                elif entry.is_file(follow_symlinks=True):
                    place_file(entry.path, os.path.join(dst, child))

    walk(src, "")


def root_has_bundle(folder):
    """True when the game folder still holds the bundle's root file or folder."""
    for name in os.listdir(folder):
        if name.casefold() in ("by.ccx", "boneyards"):
            return True
    return False


def write_record(folder, names, source):
    """Writes a content record that names every bundle file."""
    lines = ["oa-content 1", "bundle boneyards", "source " + source.replace("\n", " ")]
    for name in names:
        path = os.path.join(folder, *name.split("/"))
        digest = hashlib.sha256()
        with open(path, "rb") as handle:
            while True:
                block = handle.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
        stat = os.stat(path)
        lines.append("file " + name)
        lines.append("size %d" % stat.st_size)
        lines.append("modified %d" % int(stat.st_mtime))
        lines.append("sha256 " + digest.hexdigest())
    text = "\n".join(lines) + "\n"
    with open(os.path.join(folder, ".oa-content"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def place_alpha_map(source_game, expansion):
    """Puts the installed war map at Boneyards/alpha_map.gwm in the expansion.

    The archive beside the program does not carry it. The republished
    expansion carries alpha_map.gwm beside BYPacific_map.gwm. Until the
    bundle table lists that name, the scratch copies the installed war map
    there: alpha_map.gwm when the install has one, otherwise
    BYPacific_map.gwm. The game folder still has neither.
    """
    dest = os.path.join(expansion, "Boneyards", "alpha_map.gwm")
    if os.path.isfile(dest):
        return
    for name in ("Boneyards/alpha_map.gwm", "Boneyards/BYPacific_map.gwm"):
        found = find_source(source_game, name)
        if found is not None:
            place_file(found, dest)
            return
    raise SystemExit("the installed game has no war map for Boneyards/alpha_map.gwm")


def stage(args, names):
    """Builds the scratch user folder and the game folder that lacks the bundle."""
    work = reset_work(args.work)
    user = os.path.join(work, "user")
    expansion = os.path.join(user, "Expansions", "totala", "boneyards")
    game = os.path.join(work, "game")
    os.makedirs(expansion)
    missing = []
    for name in names:
        found = find_source(args.source_game_dir, name)
        if found is None:
            missing.append(name)
            continue
        place_file(found, os.path.join(expansion, *name.split("/")))
    if missing:
        raise SystemExit("the installed game is missing: %s" % ", ".join(missing))
    place_alpha_map(args.source_game_dir, expansion)
    write_record(expansion, names, "scratch")
    link_tree(args.source_game_dir, game, {"by.ccx", "boneyards"})
    if root_has_bundle(game):
        raise SystemExit("the scratch game folder still holds the bundle")
    if not os.path.isfile(os.path.join(expansion, "by.ccx")):
        raise SystemExit("the expansion folder has no by.ccx")
    return work, user, game


def run_game(args, work, user, game):
    """Runs the headless help check and returns its output and exit code."""
    preferences = os.path.join(work, "preferences.conf")
    command = [
        args.game,
        "--game-dir", game,
        "--user-folder", user,
        "--content", "totala:boneyards",
        "--mod-dir", args.mod_dir,
        "--skip-intro",
        "--mute",
        "--enter-boneyards", "400",
        "--boneyards-login",
        "--boneyards-planets", "25",
        "--boneyards-replay", args.replay,
        "--boneyards-open", "help",
        "--preferences-file", preferences,
        "--snapshot", os.path.join(work, "help.ppm"),
    ]
    env = os.environ.copy()
    env["SDL_VIDEO_DRIVER"] = "dummy"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    try:
        completed = subprocess.run(
            command,
            cwd=work,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        print(output, end="" if output.endswith("\n") or output == "" else "\n")
        print("native-boneyards-expansion: timed out", file=sys.stderr)
        return 1
    print(completed.stdout, end="" if completed.stdout.endswith("\n") else "\n")
    if completed.returncode != 0:
        print(
            "native-boneyards-expansion: the game exited %s" % completed.returncode,
            file=sys.stderr,
        )
        return completed.returncode
    if HELP_SHOWN not in completed.stdout or CONTENTS.search(completed.stdout) is None:
        print(
            "native-boneyards-expansion: the overlay or the help page did not open",
            file=sys.stderr,
        )
        return 1
    print("native-boneyards-expansion: passed")
    return 0


def main():
    """Stages the scratch folders and opens the overlay and a help page."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", required=True)
    parser.add_argument("--source-game-dir", required=True)
    parser.add_argument("--mod-dir", required=True)
    parser.add_argument("--bundle-table", required=True)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    names = copy_names(args.bundle_table)
    work, user, game = stage(args, names)
    return run_game(args, work, user, game)


if __name__ == "__main__":
    sys.exit(main())
