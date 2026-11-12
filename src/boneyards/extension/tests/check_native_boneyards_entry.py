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
# Boneyards with the expansion's art and sounds in use after starting again
# once, as every run that installs the package does.
#
# update: a feed offers a newer game. The run is asked nothing and goes on.
# A second entry over the recorded package downloads nothing; a changed
# catalogue package is fetched and installed once, over the copy in place,
# keeping the war map the game wrote and leaving no duplicate folder; a
# fourth entry downloads nothing again.
#
# install-id: the same entry over a registry that requires an install ID,
# with that ID off. A quiet run logs the question, keeps the off line and
# stops, naming the registry, and fetches nothing. Cancel keeps the off
# line, fetches nothing and stays on the menu. Turn on faces a player, with
# no frame limit: the question is shown, the off line is replaced by an
# install ID, the package is fetched once and the Boneyards open. With both
# checks never, that ID off and a fresh data folder, the player is asked
# the checks question and then the install-ID question, turns both on, and
# the Boneyards open the same way. The answered halves speak MCP on
# 127.0.0.1. Nothing is fetched from anywhere else.
#
# files: a player enters over a game folder that is not Open Annihilation's
# own copy and holds an archive no bundle lists. The entry is refused before
# anything is fetched: the question names the archive, Repair opens Settings
# at Game Content over the main menu, and the Boneyards do not open. The checks' other game
# trees leave such archives out.
#
# Nothing is fetched from a live server. Each run wipes its own work folder.

import argparse
import hashlib
import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse
_OA_TOOLS = Path(__file__).resolve().parents[4] / "tools"
if str(_OA_TOOLS) not in sys.path:
    sys.path.insert(0, str(_OA_TOOLS))
import native_offline  # noqa: E402

PASSPHRASE = "e06-local-pass"
REGISTRY_ID = "e06-local"
FEED_ID = "e06-feed"
ID_REGISTRY = "e01-local"
ID_NAME = "E01 Local"
ID_PASS = "e01-local-pass"
ID_KEY = "open-annihilation.install-id." + ID_REGISTRY
OTHER_ID_KEY = "open-annihilation.install-id.coreprime"
TURN_ON = "oa.expansion-services.turn-on"
CANCEL = "oa.expansion-services.cancel"
STOPPED_LOCAL = "entry: stopped: Downloads are off for E01 Local."
WOULD_ASK = "a player would be asked:"
ASKING = "entry flow: asking to turn the online features on"
STAYED = "entry flow: stayed"
CHECKS_SENTENCE = (
    "The Boneyards needs Open Annihilation's online features: update checks and the content "
    "catalogue. Turn them on and continue?"
)
INSTALL_ID_SENTENCE = (
    "The Boneyards needs " + ID_NAME + "'s install ID turned on. Turn it on and continue?"
)
INSTALL_ID_TEXT = re.compile(
    r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"
)
SERVICES_NOTE = (
    "a player would be asked: The Boneyards needs Open Annihilation's online features: update "
    "checks and the content catalogue. Turn them on and continue?"
)
UPDATE_NOTE = (
    "a player would be asked: Open Annihilation 99.0.0 is out. The Boneyards needs the latest "
    "version."
)
OPENED = "entry flow: boneyards open"
REPAIR = "oa.expansion-files.repair"
REFUSED_FILES = "entry flow: refused for the game files, offering Repair"
COPY_AGAIN = "oa.settings.content-copy.copy-again"
RESTARTED = "entry: the new package is put in use by starting again"
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


class TokensHandler(CountingHandler):
    """The direct handler, plus the download API a tokens registry requires.

    POST /v1/downloads answers 201 with one key. POST of its result answers
    204. The package file is still the GET the direct handler counts.
    """

    package_url = ""

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length > 0:
            self.rfile.read(length)
        path = self.path.split("?", 1)[0]
        if path.endswith("/result"):
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path.endswith("/downloads"):
            body = json.dumps(
                {
                    "download": "d-e01-local",
                    "expires": "2027-11-06T00:00:00Z",
                    "url": TokensHandler.package_url,
                }
            ).encode("utf-8")
            self.send_response(201)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_error(404)


def fetched_count():
    """How many package files the server has handed out."""
    with CountingHandler.lock:
        return len(CountingHandler.fetched)


def reset_fetched():
    """Forgets the package files handed out, so the next run counts its own."""
    with CountingHandler.lock:
        CountingHandler.fetched.clear()


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


def stray_archives(repo, game_dir):
    """The archives at an install's top that no bundle lists, lower case.

    The Boneyards refuses an entry over a game folder that holds one, as a
    GOG install does with its extra maps, so the checks' game tree leaves
    them out.
    """
    text = (repo / "src/data/content-bundles/src/bundle_table.inc").read_text(encoding="utf-8")
    listed = set()
    for line in text.splitlines():
        mark = 'ContentFile{"'
        at = line.find(mark)
        if at >= 0:
            rest = line[at + len(mark) :]
            listed.add(rest[: rest.index('"')].lower())
    strays = set()
    for entry in Path(game_dir).iterdir():
        name = entry.name.lower()
        archive = name.endswith((".hpi", ".ufo", ".ccx", ".gp3"))
        if entry.is_file() and archive and name not in listed:
            strays.add(name)
    return strays


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


def serve(directory, handler=CountingHandler):
    """Serves directory on 127.0.0.1 and returns the server."""
    bound = partial(handler, directory=str(directory))
    server = ThreadingHTTPServer(("127.0.0.1", 0), bound)
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


def registries_text(base, public, registry_id=REGISTRY_ID, name="E06 Local", downloads=None):
    """One local registry, with the built-in registry turned off."""
    if downloads is None:
        downloads = '    downloads:\n      mode: "direct"\n      install-id: "none"\n'
    return (
        "registries:\n"
        f'  - id: "{registry_id}"\n'
        f'    name: "{name}"\n'
        f'    catalogue: "{base}/v1/catalogue.json"\n'
        "    trusted-keys:\n"
        f'      - id: "{registry_id}"\n'
        f'        public: "{public}"\n'
        + downloads
        + '    added: "2026-11-06"\n'
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


def publish_feed(tool, serve_dir, key, passphrase, base, version="99.0.0"):
    """Signs a feed and returns its address and public key."""
    files = serve_dir / "files"
    files.mkdir(parents=True, exist_ok=True)
    packages = []
    for platform in PLATFORMS:
        package = files / f"open-annihilation-{version}-{platform}.zip"
        package.write_bytes(b"x")
        packages.append(str(package))
    feed = serve_dir / "feed.json"
    require(
        run(
            [
                tool,
                "engine-feed",
                "write",
                "--version=" + version,
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

    def __init__(self, arguments, work, tokens=False):
        self.arguments = arguments
        self.work = work
        self.tokens = tokens
        self.registry_id = ID_REGISTRY if tokens else REGISTRY_ID
        self.registry_name = ID_NAME if tokens else "E06 Local"
        self.serve_dir = work / "serve"
        self.serve_dir.mkdir(parents=True)
        self.server = serve(self.serve_dir, TokensHandler if tokens else CountingHandler)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        self.passphrase = work / "passphrase"
        self.passphrase.write_text((ID_PASS if tokens else PASSPHRASE) + "\n", encoding="utf-8")
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
                    self.registry_id,
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
                    self.registry_id,
                    "--name",
                    self.registry_name,
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
        if tokens:
            downloads = (
                "    downloads:\n"
                '      mode: "tokens"\n'
                f'      api: "{self.base}/v1"\n'
                '      install-id: "required"\n'
            )
        else:
            downloads = None
        user = work / "user"
        user.mkdir(parents=True)
        (work / "data").mkdir(parents=True)
        (user / "Registries.yaml").write_text(
            registries_text(
                self.base, public_line(made), self.registry_id, self.registry_name, downloads
            ),
            encoding="utf-8",
        )
        self.folder = user / "Expansions" / "totala" / "boneyards"
        self.game_tree = work / "game"
        link_tree(
            arguments.game_dir,
            self.game_tree,
            {"by.ccx", "boneyards"} | stray_archives(self.repo, arguments.game_dir),
        )

    def publish(self, revision, release):
        """Packs and lists one package; returns its SHA-256."""
        package = pack_expansion(
            self.arguments.tool, self.arguments.game_dir, self.work, self.names, revision
        )
        digest = publish_catalogue(
            self.arguments.tool, self.serve_dir, self.key, self.passphrase, package, release
        )
        if self.tokens:
            TokensHandler.package_url = self.base + "/v1/p/" + digest + ".oaexp"
        return digest

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def check_install(fixture):
    """A first install, asked nothing, in use once the run has started again."""
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
    if RESTARTED not in output or output.index(RESTARTED) > output.index(OPENED):
        return fail("the run did not start again before the Boneyards opened")
    print("boneyards entry check: install: fetched once, recorded, and in use after starting again")
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


def write_id_preferences(path, checks_off=False):
    """The picked registry's install ID off, and another's. Both checks never, when asked."""
    text = (
        "open-annihilation-preferences 1\n"
        '"open-annihilation.language" "en"\n'
        '"open-annihilation.first-run" "done"\n'
    )
    if checks_off:
        text += f'"{UPDATE_KEY}" "never"\n"{CATALOGUE_KEY}" "never"\n'
    text += f'"{ID_KEY}" "off"\n"{OTHER_ID_KEY}" "off"\n'
    path.write_text(text, encoding="utf-8")


def game_env(tmp):
    """The dummy drivers, and a scratch directory for the game's own files."""
    env = os.environ.copy()
    env["SDL_VIDEO_DRIVER"] = "dummy"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    env["TMPDIR"] = str(tmp)
    return env


def prepare_run(fixture, name, checks_off=False):
    """One run's folders. The data folder is fresh. The registry file is the fixture's."""
    run_dir = fixture.work / name
    user = run_dir / "user"
    data = run_dir / "data"
    tmp = run_dir / "tmp"
    user.mkdir(parents=True)
    data.mkdir()
    tmp.mkdir()
    shutil.copy(fixture.work / "user" / "Registries.yaml", user / "Registries.yaml")
    prefs = run_dir / "prefs.conf"
    write_id_preferences(prefs, checks_off)
    install = run_dir / "update-install"
    install.mkdir()
    return user, data, tmp, prefs, install


def feed_arguments(fixture, install):
    """A local feed older than this game, so the entry is not offered an update."""
    feed_key = fixture.work / "feed.key"
    if not feed_key.is_file():
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
        fixture.arguments.tool,
        fixture.serve_dir,
        feed_key,
        fixture.passphrase,
        fixture.base,
        "0.0.1",
    )
    return [
        "--update-feed",
        feed_url,
        "--update-key",
        FEED_ID + "=" + public,
        "--update-install-folder",
        str(install),
        "--boneyards-server",
        "127.0.0.1",
    ]


def message_of(content_type, payload):
    """Returns the JSON-RPC messages in one HTTP body.

    A body that is not JSON raises RuntimeError, so the check reports that
    line instead of a traceback.
    """
    text = payload.decode("utf-8", "replace")

    def one(body):
        try:
            return json.loads(body)
        except json.JSONDecodeError as error:
            shown = " ".join(body.split())[:160]
            raise RuntimeError("the MCP answer was not JSON (" + shown + ")") from error

    if "text/event-stream" in content_type:
        messages = []
        for line in text.splitlines():
            if line.startswith("data:"):
                value = line[5:].lstrip()
                if value:
                    messages.append(one(value))
        return messages
    if not text:
        return []
    return [one(text)]


def post_mcp(host, port, token, body, session_id, timeout):
    """Posts one JSON-RPC message. A held call stays open until it finishes."""
    connection = http.client.HTTPConnection(host, port, timeout=timeout)
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
        "Authorization": "Bearer " + token,
    }
    if session_id:
        headers["Mcp-Session-Id"] = session_id
    try:
        connection.request("POST", "/mcp", body=json.dumps(body), headers=headers)
        response = connection.getresponse()
        payload = response.read()
        status = response.status
        content_type = response.getheader("Content-Type") or ""
        session = response.getheader("Mcp-Session-Id")
    finally:
        connection.close()
    return status, session, message_of(content_type, payload)


def wait_endpoint(path, timeout):
    """Returns mcp.json once it names an address, a token and a process."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                time.sleep(0.05)
                continue
            if isinstance(data, dict) and {"address", "token", "pid"} <= data.keys():
                return data
        time.sleep(0.05)
    raise RuntimeError("mcp.json did not appear")


class Player:
    """One MCP session on the game's loopback endpoint."""

    def __init__(self, work):
        endpoint = wait_endpoint(work / "mcp.json", 120)
        parsed = urlparse(endpoint["address"])
        if parsed.hostname not in ("127.0.0.1", "::1") or parsed.path != "/mcp":
            raise RuntimeError("the MCP endpoint is not loopback /mcp")
        self.host = parsed.hostname
        self.port = parsed.port
        self.token = endpoint["token"]
        self.session = None
        self.call_id = 0
        status, session, messages = post_mcp(
            self.host,
            self.port,
            self.token,
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "boneyards-entry-install-id", "version": "0"},
                },
            },
            None,
            30,
        )
        if status != 200 or not session:
            raise RuntimeError(f"initialize answered HTTP {status}")
        self.session = session
        self.call_id = 1
        noted, _, _ = post_mcp(
            self.host,
            self.port,
            self.token,
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            self.session,
            30,
        )
        if noted != 202:
            raise RuntimeError(f"initialized answered HTTP {noted}")

    def call(self, name, arguments, timeout):
        """Calls one tool and returns its result object."""
        self.call_id += 1
        status, _, messages = post_mcp(
            self.host,
            self.port,
            self.token,
            {
                "jsonrpc": "2.0",
                "id": self.call_id,
                "method": "tools/call",
                "params": {"name": name, "arguments": arguments},
            },
            self.session,
            timeout,
        )
        if status != 200 or not messages:
            raise RuntimeError(f"{name} answered HTTP {status}")
        result = None
        for message in messages:
            if message.get("error"):
                raise RuntimeError(f"{name} failed: {message['error']}")
            if "result" in message:
                result = message["result"]
        if result is None:
            raise RuntimeError(f"{name} had no result")
        if result.get("isError"):
            text = ((result.get("content") or [{}])[0]).get("text") or "the tool failed"
            raise RuntimeError(f"{name}: {text}")
        return result


def read_log(path):
    """The game's log so far. A file the game still holds is read as it stands."""
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def wait_log(path, process, marker, timeout):
    """Waits until the log holds marker, or the game has exited."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = read_log(path)
        if marker in text:
            return text
        if process.poll() is not None:
            return text
        time.sleep(0.2)
    return read_log(path)


def stop_player(process, work, log_path, player=None):
    """Asks the game to quit through the session this run holds, then ends it.

    A second session is refused while one is open, so a new one is opened
    only when the call through the session already held fails.
    """
    sent = False
    if player is not None:
        try:
            player.call("game.quit", {}, 30)
            sent = True
        except (RuntimeError, OSError, http.client.HTTPException, json.JSONDecodeError):
            sent = False
    if not sent:
        try:
            Player(work).call("game.quit", {}, 30)
        except (RuntimeError, OSError, http.client.HTTPException, json.JSONDecodeError):
            pass
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)
    return read_log(log_path)


def run_quiet(arguments, game_tree, run_dir, user, data, tmp, prefs, extra):
    """One unattended entry. --frames makes it so, and -y starts it."""
    command = [
        arguments.game,
        "--game-dir",
        str(game_tree),
        "--skip-intro",
        "--mute",
        "-y",
        "--frames",
        "9600",
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(prefs),
        *extra,
    ]
    completed = subprocess.run(
        command,
        cwd=str(run_dir),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=game_env(tmp),
    )
    sys.stdout.write(completed.stdout)
    sys.stdout.flush()
    if completed.returncode != 0:
        print(f"boneyards entry check: the game exited {completed.returncode}", file=sys.stderr)
        return None
    return completed.stdout


def run_facing(arguments, game_tree, run_dir, user, data, tmp, prefs, extra, answer):
    """One entry a player answers over MCP. No frame limit and no check flag."""
    command = [
        arguments.game,
        "--game-dir",
        str(game_tree),
        "--skip-intro",
        "--mute",
        "--mcp-http",
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(prefs),
        *extra,
    ]
    log_path = run_dir / "game.log"
    log_handle = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command,
        cwd=str(run_dir),
        stdout=log_handle,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        env=game_env(tmp),
    )
    output = ""
    player = None
    try:
        player = Player(run_dir)
        player.call("ui.wait", {"screen": "main_menu", "timeout_ms": 120000}, 150)
        player.call("ui.key", {"chord": "B", "phase": "tap"}, 30)
        try:
            player.call("ui.wait", {"control": TURN_ON, "timeout_ms": 90000}, 120)
        except RuntimeError:
            player.call("ui.key", {"chord": "B", "phase": "tap"}, 30)
            player.call("ui.wait", {"control": TURN_ON, "timeout_ms": 30000}, 60)
        button = TURN_ON if answer == "turn-on" else CANCEL
        player.call("ui.click", {"name": button}, 30)
        if answer == "turn-on":
            output = wait_log(log_path, process, OPENED, 240)
        else:
            output = wait_log(log_path, process, STAYED, 30)
        output = stop_player(process, run_dir, log_path, player)
    except (
        RuntimeError,
        OSError,
        http.client.HTTPException,
        json.JSONDecodeError,
        subprocess.TimeoutExpired,
    ) as error:
        output = stop_player(process, run_dir, log_path, player)
        sys.stdout.write(output)
        sys.stdout.flush()
        print(f"boneyards entry check: {error}", file=sys.stderr)
        return None
    finally:
        log_handle.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
    sys.stdout.write(output)
    sys.stdout.flush()
    if process.returncode not in (0, None):
        print(f"boneyards entry check: the game exited {process.returncode}", file=sys.stderr)
        return None
    return output


def run_files(arguments, game_tree, run_dir, user, data, tmp, prefs, extra):
    """One entry a player makes over MCP, refused for the game files: Repair."""
    command = [
        arguments.game,
        "--game-dir",
        str(game_tree),
        "--skip-intro",
        "--mute",
        "--mcp-http",
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(prefs),
        *extra,
    ]
    log_path = run_dir / "game.log"
    log_handle = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command,
        cwd=str(run_dir),
        stdout=log_handle,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        env=game_env(tmp),
    )
    output = ""
    player = None
    try:
        player = Player(run_dir)
        player.call("ui.wait", {"screen": "main_menu", "timeout_ms": 120000}, 150)
        player.call("ui.key", {"chord": "B", "phase": "tap"}, 30)
        try:
            player.call("ui.wait", {"control": REPAIR, "timeout_ms": 90000}, 120)
        except RuntimeError:
            player.call("ui.key", {"chord": "B", "phase": "tap"}, 30)
            player.call("ui.wait", {"control": REPAIR, "timeout_ms": 30000}, 60)
        player.call("ui.click", {"name": REPAIR}, 30)
        # Game Content's own buttons are listed only while its page is open;
        # they may lie below the fold, so they are looked for, not waited on.
        deadline = time.monotonic() + 30
        while COPY_AGAIN not in player.call("ui.tree", {}, 30)["content"][0]["text"]:
            if time.monotonic() > deadline:
                raise RuntimeError("Repair did not open Settings at Game Content")
            time.sleep(0.5)
        output = stop_player(process, run_dir, log_path, player)
    except (
        RuntimeError,
        OSError,
        http.client.HTTPException,
        json.JSONDecodeError,
        subprocess.TimeoutExpired,
    ) as error:
        output = stop_player(process, run_dir, log_path, player)
        sys.stdout.write(output)
        sys.stdout.flush()
        print(f"boneyards entry check: {error}", file=sys.stderr)
        return None
    finally:
        log_handle.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
    sys.stdout.write(output)
    sys.stdout.flush()
    if process.returncode not in (0, None):
        print(f"boneyards entry check: the game exited {process.returncode}", file=sys.stderr)
        return None
    return output


def check_files(fixture):
    """A stray archive refuses the entry, names itself, and Repair opens Game Content."""
    fixture.publish(1, 1)
    tree = fixture.work / "stray-game"
    link_tree(fixture.arguments.game_dir, tree, {"by.ccx", "boneyards"})
    # One stray of the check's own, whatever the install holds beside it.
    found = [e for e in fixture.arguments.game_dir.iterdir() if e.name.lower() == "afark.ufo"]
    source = found[0] if found else None
    if source is None:
        return fail("files: the install has no AFark.ufo to copy as a stray archive")
    shutil.copy2(source, tree / "rgstray.ufo")
    strays = stray_archives(fixture.repo, tree)
    reset_fetched()
    user, data, tmp, prefs, install = prepare_run(fixture, "files")
    extra = feed_arguments(fixture, install)
    output = run_files(
        fixture.arguments, tree, fixture.work / "files", user, data, tmp, prefs, extra
    )
    if output is None:
        return fail("files: the game did not finish")
    if REFUSED_FILES not in output:
        return fail("files: the entry was not refused for the game files")
    named = re.search(
        r"entry: stopped: (\S+) in .* is not one of Total Annihilation's files", output
    )
    if named is None or named.group(1).lower() not in strays:
        return fail("files: the question did not name a stray archive")
    if "copy it from your own install or CD to enter" not in output:
        return fail("files: the question did not say how to repair it")
    if OPENED in output:
        return fail("files: the Boneyards opened")
    if fetched_count() != 0:
        return fail(f"files: the package was fetched {fetched_count()} times")
    print(
        f"boneyards entry check: files: {named.group(1)} refused the entry, the question named "
        "it, and Repair opened Game Content"
    )
    return 0


def wait_sentence(player, process, log_path, sentence, timeout):
    """Waits until the log holds sentence and Turn on is on the screen."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("the game exited before the question")
        if sentence in read_log(log_path):
            try:
                player.call("ui.wait", {"control": TURN_ON, "timeout_ms": 3000}, 20)
                return
            except RuntimeError:
                pass
        time.sleep(0.2)
    raise RuntimeError("the question was not shown")


def run_checks_then_id(arguments, game_tree, run_dir, user, data, tmp, prefs, extra):
    """Both checks off and no list kept: the checks question, then the install ID.

    The player turns each on. No frame limit and no check flag. Quit goes
    through the session this run already holds.
    """
    command = [
        arguments.game,
        "--game-dir",
        str(game_tree),
        "--skip-intro",
        "--mute",
        "--mcp-http",
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(prefs),
        *extra,
    ]
    log_path = run_dir / "game.log"
    log_handle = log_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command,
        cwd=str(run_dir),
        stdout=log_handle,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        env=game_env(tmp),
    )
    output = ""
    player = None
    try:
        player = Player(run_dir)
        player.call("ui.wait", {"screen": "main_menu", "timeout_ms": 120000}, 150)
        player.call("ui.key", {"chord": "B", "phase": "tap"}, 30)
        try:
            wait_sentence(player, process, log_path, CHECKS_SENTENCE, 90)
        except RuntimeError:
            player.call("ui.key", {"chord": "B", "phase": "tap"}, 30)
            wait_sentence(player, process, log_path, CHECKS_SENTENCE, 45)
        if INSTALL_ID_SENTENCE in read_log(log_path):
            raise RuntimeError("the install-ID question came before the checks question")
        player.call("ui.click", {"name": TURN_ON}, 30)
        wait_sentence(player, process, log_path, INSTALL_ID_SENTENCE, 180)
        player.call("ui.click", {"name": TURN_ON}, 30)
        output = wait_log(log_path, process, OPENED, 240)
        output = stop_player(process, run_dir, log_path, player)
    except (
        RuntimeError,
        OSError,
        http.client.HTTPException,
        json.JSONDecodeError,
        subprocess.TimeoutExpired,
    ) as error:
        output = stop_player(process, run_dir, log_path, player)
        sys.stdout.write(output)
        sys.stdout.flush()
        print(f"boneyards entry check: {error}", file=sys.stderr)
        return None
    finally:
        log_handle.close()
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
    sys.stdout.write(output)
    sys.stdout.flush()
    if process.returncode not in (0, None):
        print(f"boneyards entry check: the game exited {process.returncode}", file=sys.stderr)
        return None
    return output


def question_names_install_id(output):
    """True when a logged or shown question names this registry's install ID."""
    for line in output.splitlines():
        if "install ID" in line and ID_NAME in line:
            return True
    return False


def check_install_id(fixture):
    """The install ID off: a quiet stop, Cancel, Turn on, and both questions."""
    fixture.publish(1, 1)
    if not TokensHandler.package_url:
        return fail("the download key has no package address")

    reset_fetched()
    user, data, tmp, prefs, install = prepare_run(fixture, "quiet")
    extra = feed_arguments(fixture, install)
    output = run_quiet(
        fixture.arguments, fixture.game_tree, fixture.work / "quiet", user, data, tmp, prefs, extra
    )
    if output is None:
        return fail("quiet: the game did not finish")
    if not question_names_install_id(output) or WOULD_ASK not in output:
        return fail("quiet: the logged question did not name the install ID")
    if STOPPED_LOCAL not in output:
        return fail("quiet: the stop did not name E01 Local")
    if fetched_count() != 0:
        return fail(f"quiet: the package was fetched {fetched_count()} times")
    if stored_value(prefs, ID_KEY) != "off" or stored_value(prefs, OTHER_ID_KEY) != "off":
        return fail("quiet: the off line changed")
    if OPENED in output:
        return fail("quiet: the Boneyards opened")
    print("boneyards entry check: install-id: quiet logged the question and stopped")

    reset_fetched()
    user, data, tmp, prefs, install = prepare_run(fixture, "cancel")
    extra = feed_arguments(fixture, install)
    output = run_facing(
        fixture.arguments,
        fixture.game_tree,
        fixture.work / "cancel",
        user,
        data,
        tmp,
        prefs,
        extra,
        "cancel",
    )
    if output is None:
        return fail("cancel: the game did not finish")
    if ASKING not in output or not question_names_install_id(output):
        return fail("cancel: Turn on was not asked")
    if WOULD_ASK in output:
        return fail("cancel: the question was only logged")
    if STAYED not in output:
        return fail("cancel: the entry did not stay on the menu")
    if STOPPED_LOCAL in output or OPENED in output:
        return fail("cancel: the entry stopped or opened")
    if fetched_count() != 0:
        return fail(f"cancel: the package was fetched {fetched_count()} times")
    if stored_value(prefs, ID_KEY) != "off" or stored_value(prefs, OTHER_ID_KEY) != "off":
        return fail("cancel: the off line changed")
    print("boneyards entry check: install-id: cancel changed nothing")

    reset_fetched()
    user, data, tmp, prefs, install = prepare_run(fixture, "turn-on")
    extra = feed_arguments(fixture, install)
    output = run_facing(
        fixture.arguments,
        fixture.game_tree,
        fixture.work / "turn-on",
        user,
        data,
        tmp,
        prefs,
        extra,
        "turn-on",
    )
    if output is None:
        return fail("turn on: the game did not finish")
    if ASKING not in output or not question_names_install_id(output):
        return fail("turn on: Turn on was not asked")
    if WOULD_ASK in output:
        return fail("turn on: the question was only logged")
    if "entry flow: asking to update" in output:
        return fail("turn on: the feed offered an update")
    if STOPPED_LOCAL in output or "entry: stopped:" in output:
        return fail("turn on: the entry stopped")
    if OPENED not in output:
        return fail("turn on: the Boneyards did not open")
    if fetched_count() != 1:
        return fail(f"turn on: the package was fetched {fetched_count()} times, not once")
    made = stored_value(prefs, ID_KEY)
    if made is None or not INSTALL_ID_TEXT.match(made):
        return fail("turn on: no install ID was stored")
    if stored_value(prefs, OTHER_ID_KEY) != "off":
        return fail("turn on: another registry's off line changed")
    print(
        "boneyards entry check: install-id: turn on stored an ID, fetched once, and opened"
    )

    reset_fetched()
    user, data, tmp, prefs, install = prepare_run(fixture, "checks-off", checks_off=True)
    extra = feed_arguments(fixture, install)
    output = run_checks_then_id(
        fixture.arguments,
        fixture.game_tree,
        fixture.work / "checks-off",
        user,
        data,
        tmp,
        prefs,
        extra,
    )
    if output is None:
        return fail("checks off: the game did not finish")
    checks_at = output.find(CHECKS_SENTENCE)
    install_at = output.find(INSTALL_ID_SENTENCE)
    if output.count(ASKING) < 2 or checks_at < 0 or install_at < checks_at:
        return fail("checks off: the log did not ask the checks question, then the install ID")
    if WOULD_ASK in output:
        return fail("checks off: a question was only logged")
    if "entry flow: asking to update" in output:
        return fail("checks off: the feed offered an update")
    if STOPPED_LOCAL in output or "entry: stopped:" in output:
        return fail("checks off: the entry stopped")
    if OPENED not in output:
        return fail("checks off: the Boneyards did not open")
    if fetched_count() != 1:
        return fail(f"checks off: the package was fetched {fetched_count()} times, not once")
    if (
        stored_value(prefs, UPDATE_KEY) != "automatically"
        or stored_value(prefs, CATALOGUE_KEY) != "automatically"
    ):
        return fail("checks off: a check was not turned on")
    made = stored_value(prefs, ID_KEY)
    if made is None or not INSTALL_ID_TEXT.match(made):
        return fail("checks off: no install ID was stored")
    if stored_value(prefs, OTHER_ID_KEY) != "off":
        return fail("checks off: another registry's off line changed")
    print(
        "boneyards entry check: install-id: checks then the install ID, both turned on, "
        "fetched once, and opened"
    )
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--game-dir", required=True, type=Path)
    parser.add_argument(
        "--mode", required=True, choices=("install", "update", "install-id", "files")
    )
    parser.add_argument("--work", required=True, type=Path)
    arguments = parser.parse_args()
    if arguments.work.exists():
        shutil.rmtree(arguments.work)
    arguments.work.mkdir(parents=True)
    fixture = None
    try:
        fixture = Fixture(arguments, arguments.work, tokens=arguments.mode == "install-id")
        if arguments.mode == "install":
            return check_install(fixture)
        if arguments.mode == "install-id":
            return check_install_id(fixture)
        if arguments.mode == "files":
            return check_files(fixture)
        return check_update(fixture)
    except (RuntimeError, json.JSONDecodeError) as error:
        return fail(str(error))
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == "__main__":
    sys.exit(main())
