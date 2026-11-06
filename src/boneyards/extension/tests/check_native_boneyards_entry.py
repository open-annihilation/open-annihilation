#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# ENTER on the main menu, as a quiet run, over a registry on this machine
# that lists the Boneyards expansion as the catalogue does: one .oaexp that
# holds the archive beside the Boneyards folder, whose size and SHA-256 are
# the whole package's. The game folder holds neither, so what the Boneyards
# read once they open comes from the installed expansion.
#
# install: both online checks are off. The run is asked nothing, changes
# neither setting, fetches the package once, records it, and opens the
# Boneyards with the expansion's art and sounds in use in the same run.
#
# update: a feed offers a newer game. The run is asked nothing and goes on.
# A second entry over the recorded package downloads nothing; a changed
# catalogue package is fetched and installed once, over the copy in place,
# keeping the war map the game wrote and leaving no duplicate folder; a
# fourth entry downloads nothing again.
#
# Nothing is fetched from a live server. Each run wipes its own work folder.

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PASSPHRASE = "e06-local-pass"
REGISTRY_ID = "e06-local"
FEED_ID = "e06-feed"
SERVICES_NOTE = (
    "a player would be asked: The Boneyards needs Open Annihilation's online features: update "
    "checks and the content catalogue. Turn them on and continue?"
)
UPDATE_NOTE = (
    "a player would be asked: Open Annihilation 99.0.0 is out. The Boneyards needs the latest "
    "version."
)
OPENED = "entry flow: boneyards open"
IN_USE = "entry check: the Boneyards opened with the expansion in use"
READ_FROM = "are read from the installed expansion"
UPDATE_KEY = "open-annihilation.update-check"
CATALOGUE_KEY = "open-annihilation.content-updates"
WAR_MAP = b"a war map the game wrote"
PLATFORMS = (
    "linux-arm64",
    "linux-armhf",
    "linux-x86_64",
    "macos-universal",
    "steam-deck",
    "windows-95",
    "windows-arm64",
    "windows-x64",
    "windows-x86",
)


class CountingHandler(SimpleHTTPRequestHandler):
    """Serves one folder, writes nothing, and counts each package fetched."""

    fetched = []
    lock = threading.Lock()

    def do_GET(self):
        if self.path.endswith(".oaexp"):
            with CountingHandler.lock:
                CountingHandler.fetched.append(self.path)
        return super().do_GET()

    def log_message(self, format, *args):
        return


def fetched_count():
    """How many package files the server has handed out."""
    with CountingHandler.lock:
        return len(CountingHandler.fetched)


def fail(message):
    """Prints a failure and returns the failed status."""
    print("boneyards entry check: " + message, file=sys.stderr)
    return 1


def run(command, cwd=None):
    """Runs one command. The output is kept. No shell is used."""
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def require(completed, what):
    """Returns the command's output, or raises after printing why it stopped."""
    if completed.returncode == 0:
        return completed.stdout
    sys.stderr.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    raise RuntimeError(what + " failed")


def public_line(text):
    """The `public:` line a keygen prints, as `ed25519:...`."""
    for line in text.splitlines():
        if line.startswith("public:"):
            return line.split(":", 1)[1].strip()
    raise RuntimeError("keygen did not print a public key")


def link_tree(source, destination, skip_root):
    """Links every file. A root name in skip_root is left out. A link that
    the volume refuses becomes a symlink, then a copy."""
    source = source.resolve()
    for dirpath, dirnames, filenames in os.walk(source):
        relative = Path(dirpath).relative_to(source)
        if relative == Path("."):
            dirnames[:] = [name for name in dirnames if name.lower() not in skip_root]
        target_dir = destination / relative
        target_dir.mkdir(parents=True, exist_ok=True)
        for name in filenames:
            if relative == Path(".") and name.lower() in skip_root:
                continue
            src = Path(dirpath) / name
            dst = target_dir / name
            try:
                os.link(src, dst)
                continue
            except OSError:
                pass
            try:
                os.symlink(src, dst)
                continue
            except OSError:
                shutil.copy2(src, dst, follow_symlinks=True)


def game_index(root):
    """Lower-case relative paths of the files under root."""
    index = {}
    for dirpath, _dirnames, filenames in os.walk(root):
        relative = Path(dirpath).relative_to(root)
        for name in filenames:
            key = (relative / name).as_posix().lower()
            index.setdefault(key, Path(dirpath) / name)
    return index


def bundle_names(repo):
    """The copy names of the Boneyards expansion, in table order."""
    text = (repo / "src/data/content-bundles/src/bundle_table.inc").read_text(encoding="utf-8")
    start = text.index("constexpr ContentFile k_files_boneyards[]")
    end = text.index("\n};", start)
    names = []
    for line in text[start:end].splitlines():
        mark = 'ContentFile{"'
        at = line.find(mark)
        if at < 0:
            continue
        rest = line[at + len(mark) :]
        names.append(rest[: rest.index('"')])
    if not names:
        raise RuntimeError("the Boneyards file table is empty")
    return names


def stage_expansion(game_dir, folder, names, revision):
    """Copies the expansion's files from the installed game and writes its manifest."""
    index = game_index(game_dir)
    for name in names:
        exact = game_dir / name
        found = exact if exact.is_file() else index.get(name.lower())
        if found is None:
            raise RuntimeError("the game folder has no " + name)
        destination = folder / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(found, destination, follow_symlinks=True)
    manifest = (
        "oaexp: 1\n"
        "id: boneyards\n"
        "name: The Boneyards\n"
        'version: "1"\n'
        "extends:\n"
        "  - totala\n"
        "needs: []\n"
        "author:\n"
        "  name: Ridge\n"
        "  email: ridge@example.com\n"
        "packaging:\n"
        f"  revision: {revision}\n"
        '  date: "2026-11-06"\n'
        "  packager: Ridge\n"
    )
    (folder / "oaexp.yaml").write_text(manifest, encoding="utf-8")


def pack_expansion(tool, game_dir, work, names, revision):
    """Packs the expansion as the catalogue lists it, and returns the package."""
    folder = work / f"expansion-{revision}"
    stage_expansion(game_dir, folder, names, revision)
    packed = work / f"boneyards-{revision}.oaexp"
    require(run([tool, "pack", str(folder), "--out", str(packed), "--force"]), "pack")
    return packed


def serve(directory):
    """Serves directory on 127.0.0.1 and returns the server."""
    handler = partial(CountingHandler, directory=str(directory))
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def write_preferences(path, checks_off):
    """A finished welcome, in English, with both online checks off when asked."""
    text = (
        "open-annihilation-preferences 1\n"
        '"open-annihilation.language" "en"\n'
        '"open-annihilation.first-run" "done"\n'
    )
    if checks_off:
        text += f'"{UPDATE_KEY}" "never"\n"{CATALOGUE_KEY}" "never"\n'
    path.write_text(text, encoding="utf-8")


def stored_value(path, key):
    """The value a preferences file stores for key, or None."""
    marker = f'"{key}" '
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith(marker):
            return line[len(marker) :].strip().strip('"')
    return None


def registries_text(base, public):
    """One local registry, with the built-in registry turned off."""
    return (
        "registries:\n"
        f'  - id: "{REGISTRY_ID}"\n'
        '    name: "E06 Local"\n'
        f'    catalogue: "{base}/v1/catalogue.json"\n'
        "    trusted-keys:\n"
        f'      - id: "{REGISTRY_ID}"\n'
        f'        public: "{public}"\n'
        "    downloads:\n"
        '      mode: "direct"\n'
        '      install-id: "none"\n'
        '    added: "2026-11-06"\n'
        "    enabled: true\n"
        "disabled:\n"
        '  - "coreprime"\n'
    )


def publish_catalogue(tool, serve_dir, key, passphrase, package, release):
    """Lists the packed expansion in the catalogue, signed again, and returns its SHA-256."""
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    catalogue_path = serve_dir / "v1" / "catalogue.json"
    catalogue = json.loads(catalogue_path.read_text(encoding="utf-8"))
    catalogue["sequence"] = int(catalogue["sequence"]) + 1
    catalogue["generated"] = "2026-11-06T00:00:00Z"
    catalogue["expires"] = "2027-11-06T00:00:00Z"
    catalogue["packages"] = [
        {
            "kind": "expansion",
            "id": "boneyards",
            "name": "The Boneyards",
            "version": "1",
            "release": release,
            "size": package.stat().st_size,
            "sha256": digest,
            "file": "/v1/p/" + digest + ".oaexp",
            "extends": ["totala"],
        }
    ]
    catalogue_path.write_text(json.dumps(catalogue) + "\n", encoding="utf-8")
    filed = serve_dir / "v1" / "p" / (digest + ".oaexp")
    filed.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(package, filed)
    require(
        run(
            [
                tool,
                "catalogue",
                "sign",
                "--key",
                str(key),
                "--passphrase-file",
                str(passphrase),
                str(catalogue_path),
            ]
        ),
        "catalogue sign",
    )
    return digest


def publish_feed(tool, serve_dir, key, passphrase, base):
    """Signs a feed of version 99.0.0 and returns its address and public key."""
    files = serve_dir / "files"
    files.mkdir(parents=True, exist_ok=True)
    packages = []
    for platform in PLATFORMS:
        package = files / f"open-annihilation-99.0.0-{platform}.zip"
        package.write_bytes(b"x")
        packages.append(str(package))
    feed = serve_dir / "feed.json"
    require(
        run(
            [
                tool,
                "engine-feed",
                "write",
                "--version=99.0.0",
                "--date",
                "2026-11-06",
                "--from",
                "0.0.1",
                "--page",
                base + "/page",
                "--mirror",
                base + "/files/",
                "--out",
                str(feed),
                *packages,
            ]
        ),
        "engine-feed write",
    )
    require(
        run(
            [
                tool,
                "engine-feed",
                "sign",
                "--key",
                str(key),
                "--passphrase-file",
                str(passphrase),
                str(feed),
            ]
        ),
        "engine-feed sign",
    )
    made = require(run([tool, "catalogue", "public", str(key)]), "catalogue public")
    return base + "/feed.json", public_line(made)


def run_game(arguments, game_tree, work, extra):
    """Starts one quiet entry and returns its output, or None when the game failed."""
    user = work / "user"
    data = work / "data"
    command = [
        arguments.game,
        "--game-dir",
        str(game_tree),
        "--skip-intro",
        "--mute",
        "--check-boneyards-entry",
        arguments.mode,
        "--frames",
        "9600",
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(work / "prefs.conf"),
        *extra,
    ]
    env = os.environ.copy()
    env["SDL_VIDEO_DRIVER"] = "dummy"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    completed = subprocess.run(
        command,
        cwd=str(work),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
    )
    sys.stdout.write(completed.stdout)
    sys.stdout.flush()
    if completed.returncode != 0:
        print(f"boneyards entry check: the game exited {completed.returncode}", file=sys.stderr)
        return None
    return completed.stdout


def opened_with_expansion(output):
    """Why an entry did not open with the expansion in use, or None when it did."""
    if output is None:
        return "the game did not finish"
    if "entry flow: failed" in output:
        return "the entry failed"
    if "entry flow: asking" in output:
        return "a quiet run was asked a question"
    if OPENED not in output:
        return "the Boneyards did not open"
    if READ_FROM not in output or IN_USE not in output:
        return "the Boneyards did not read the installed expansion"
    for line in output.splitlines():
        if line.startswith("sound unavailable:"):
            return "a sound was not found: " + line
    return None


def recorded_package(folder):
    """The package line of the expansion's record, as (sha256, release), or None."""
    record = folder / ".oa-content"
    if not record.is_file():
        return None
    for line in record.read_text(encoding="utf-8").splitlines():
        if line.startswith("package "):
            parts = line.split(" ")
            if len(parts) == 3:
                return parts[1], int(parts[2])
    return None


def duplicate_folders(folder):
    """Folders a file sync or a swap would leave: numbered copies and set-aside folders."""
    found = []
    for path in folder.parent.iterdir():
        if path.name.startswith(".oaexp-"):
            found.append(path.name)
    for path in (folder / "Boneyards").parent.iterdir():
        if path.is_dir() and path.name.startswith("Boneyards ") and path.name[10:].isdigit():
            found.append(path.name)
    return found


class Fixture:
    """The registry, its key and the game tree the runs share."""

    def __init__(self, arguments, work):
        self.arguments = arguments
        self.work = work
        self.serve_dir = work / "serve"
        self.serve_dir.mkdir(parents=True)
        self.server = serve(self.serve_dir)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.passphrase = work / "passphrase"
        self.passphrase.write_text(PASSPHRASE + "\n", encoding="utf-8")
        self.repo = Path(__file__).resolve().parents[4]
        self.names = bundle_names(self.repo)
        self.key = work / "registry.key"
        made = require(
            run(
                [
                    arguments.tool,
                    "catalogue",
                    "keygen",
                    "--id",
                    REGISTRY_ID,
                    "--out",
                    str(self.key),
                    "--passphrase-file",
                    str(self.passphrase),
                ]
            ),
            "catalogue keygen",
        )
        require(
            run(
                [
                    arguments.tool,
                    "registry",
                    "init",
                    str(self.serve_dir),
                    "--id",
                    REGISTRY_ID,
                    "--name",
                    "E06 Local",
                    "--base-url",
                    self.base,
                    "--key",
                    str(self.key),
                    "--passphrase-file",
                    str(self.passphrase),
                ]
            ),
            "registry init",
        )
        user = work / "user"
        user.mkdir(parents=True)
        (work / "data").mkdir(parents=True)
        (user / "Registries.yaml").write_text(
            registries_text(self.base, public_line(made)), encoding="utf-8"
        )
        self.folder = user / "Expansions" / "totala" / "boneyards"
        self.game_tree = work / "game"
        link_tree(arguments.game_dir, self.game_tree, {"by.ccx", "boneyards"})

    def publish(self, revision, release):
        """Packs and lists one package; returns its SHA-256."""
        package = pack_expansion(
            self.arguments.tool, self.arguments.game_dir, self.work, self.names, revision
        )
        return publish_catalogue(
            self.arguments.tool, self.serve_dir, self.key, self.passphrase, package, release
        )

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def check_install(fixture):
    """A first install, asked nothing, with the expansion in use in the same run."""
    digest = fixture.publish(1, 1)
    prefs = fixture.work / "prefs.conf"
    write_preferences(prefs, checks_off=True)
    output = run_game(fixture.arguments, fixture.game_tree, fixture.work, [])
    why = opened_with_expansion(output)
    if why is not None:
        return fail(why)
    if SERVICES_NOTE not in output:
        return fail("the run did not log the online features' question")
    if stored_value(prefs, UPDATE_KEY) != "never" or stored_value(prefs, CATALOGUE_KEY) != "never":
        return fail("a quiet run changed the online features' settings")
    if fetched_count() != 1:
        return fail(f"the package was fetched {fetched_count()} times, not once")
    if not (fixture.folder / "by.ccx").is_file():
        return fail("the expansion archive was not installed")
    if recorded_package(fixture.folder) != (digest, 1):
        return fail("the record does not name the package installed")
    if "boneyards" not in prefs.read_text(encoding="utf-8"):
        return fail("the expansion was not turned on")
    print("boneyards entry check: install: fetched once, recorded, and in use in the same run")
    return 0


def check_update(fixture):
    """Entries over a recorded package fetch only a changed one."""
    feed_key = fixture.work / "feed.key"
    require(
        run(
            [
                fixture.arguments.tool,
                "catalogue",
                "keygen",
                "--id",
                FEED_ID,
                "--out",
                str(feed_key),
                "--passphrase-file",
                str(fixture.passphrase),
            ]
        ),
        "feed keygen",
    )
    feed_url, public = publish_feed(
        fixture.arguments.tool, fixture.serve_dir, feed_key, fixture.passphrase, fixture.base
    )
    install = fixture.work / "update-install"
    install.mkdir()
    extra = [
        "--update-feed",
        feed_url,
        "--update-key",
        FEED_ID + "=" + public,
        "--update-install-folder",
        str(install),
    ]
    prefs = fixture.work / "prefs.conf"
    write_preferences(prefs, checks_off=False)
    first = fixture.publish(1, 1)

    output = run_game(fixture.arguments, fixture.game_tree, fixture.work, extra)
    why = opened_with_expansion(output)
    if why is not None:
        return fail("first entry: " + why)
    if UPDATE_NOTE not in output:
        return fail("the run did not log the update question")
    if stored_value(prefs, UPDATE_KEY) is not None or stored_value(prefs, CATALOGUE_KEY) is not None:
        return fail("a quiet run changed the online features' settings")
    if fetched_count() != 1 or recorded_package(fixture.folder) != (first, 1):
        return fail("the first entry did not fetch and record the package once")

    # What the game writes beside the package's own files, and an empty
    # copy a file sync left of the Boneyards folder.
    war_map = fixture.folder / "Boneyards" / "alpha_map.gwm"
    war_map.write_bytes(WAR_MAP)
    (fixture.folder / "Boneyards 2").mkdir()

    output = run_game(fixture.arguments, fixture.game_tree, fixture.work, extra)
    why = opened_with_expansion(output)
    if why is not None:
        return fail("second entry: " + why)
    if fetched_count() != 1 or "package install: opening" in output:
        return fail("the second entry fetched the package it had recorded")
    if "boneyards release 1 is installed" not in output:
        return fail("the second entry did not find the package recorded")

    second = fixture.publish(2, 2)
    if second == first:
        return fail("the changed package has the same SHA-256")
    output = run_game(fixture.arguments, fixture.game_tree, fixture.work, extra)
    why = opened_with_expansion(output)
    if why is not None:
        return fail("third entry: " + why)
    if fetched_count() != 2 or output.count("package install: opening") != 1:
        return fail("the changed package was not fetched and installed once")
    if "boneyards release 2 is fetched: the copy installed is another package" not in output:
        return fail("the third entry did not name the changed package")
    if recorded_package(fixture.folder) != (second, 2):
        return fail("the record does not name the changed package")
    if not war_map.is_file() or war_map.read_bytes() != WAR_MAP:
        return fail("the reinstall did not keep the war map the game wrote")
    duplicates = duplicate_folders(fixture.folder)
    if duplicates:
        return fail("the reinstall left " + ", ".join(sorted(duplicates)))

    output = run_game(fixture.arguments, fixture.game_tree, fixture.work, extra)
    why = opened_with_expansion(output)
    if why is not None:
        return fail("fourth entry: " + why)
    if fetched_count() != 2 or "package install: opening" in output:
        return fail("the fourth entry fetched the package it had recorded")
    print(
        "boneyards entry check: update: an equal record fetched nothing, a changed package was "
        "installed once and kept the war map"
    )
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--game-dir", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("install", "update"))
    parser.add_argument("--work", required=True, type=Path)
    arguments = parser.parse_args()
    if arguments.work.exists():
        shutil.rmtree(arguments.work)
    arguments.work.mkdir(parents=True)
    fixture = None
    try:
        fixture = Fixture(arguments, arguments.work)
        if arguments.mode == "install":
            return check_install(fixture)
        return check_update(fixture)
    except RuntimeError as error:
        return fail(str(error))
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == "__main__":
    sys.exit(main())
