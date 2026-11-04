#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# ENTER on the main menu. install fetches the Boneyards expansion from a
# registry on this machine and opens it. update is offered a newer game,
# declines, and stays on the menu. Nothing is fetched from a live server.

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

QUESTION = "A newer Open Annihilation is out. Update before entering the Boneyards?"
PASSPHRASE = "e01-local-pass"
REGISTRY_ID = "e01-local"
FEED_ID = "e01-feed"
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


class QuietHandler(SimpleHTTPRequestHandler):
    """Serves one folder and writes nothing."""

    def log_message(self, format, *args):
        return


def fail(message):
    """Prints a failure and returns the failed status."""
    print(message, file=sys.stderr)
    return 1


def run(command, cwd=None):
    """Runs one command. The output is kept. No shell is used."""
    completed = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed


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
    for dirpath, _dirnames, filenames in os.walk(source):
        relative = Path(dirpath).relative_to(source)
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


def stage_expansion(game_dir, folder, names):
    """Copies the expansion's files and writes its manifest."""
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
        'oaexp: 1\n'
        'id: boneyards\n'
        'name: The Boneyards\n'
        'version: "1"\n'
        "extends:\n"
        "  - totala\n"
        "needs: []\n"
        "author:\n"
        "  name: Ridge\n"
        "  email: ridge@example.com\n"
        "packaging:\n"
        "  revision: 1\n"
        '  date: "2026-11-02"\n'
        "  packager: Ridge\n"
    )
    (folder / "oaexp.yaml").write_text(manifest, encoding="utf-8")


def serve(directory):
    """Serves directory on 127.0.0.1 and returns the server."""
    handler = partial(QuietHandler, directory=str(directory))
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def preferences(path):
    """A finished welcome, in English."""
    path.write_text(
        "open-annihilation-preferences 1\n"
        '"open-annihilation.language" "en"\n'
        '"open-annihilation.first-run" "done"\n',
        encoding="utf-8",
    )


def registries_text(base, public):
    """One local registry, with the built-in registry turned off."""
    return (
        "registries:\n"
        f'  - id: "{REGISTRY_ID}"\n'
        '    name: "E01 Local"\n'
        f'    catalogue: "{base}/v1/catalogue.json"\n'
        "    trusted-keys:\n"
        f'      - id: "{REGISTRY_ID}"\n'
        f'        public: "{public}"\n'
        "    downloads:\n"
        '      mode: "direct"\n'
        '      install-id: "none"\n'
        '    added: "2026-11-02"\n'
        "    enabled: true\n"
        "disabled:\n"
        '  - "coreprime"\n'
    )


def publish_catalogue(tool, serve_dir, key, passphrase, package):
    """Puts the packed expansion in the catalogue and signs it again."""
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    catalogue_path = serve_dir / "v1" / "catalogue.json"
    catalogue = json.loads(catalogue_path.read_text(encoding="utf-8"))
    catalogue["sequence"] = int(catalogue["sequence"]) + 1
    catalogue["generated"] = "2026-11-02T00:00:00Z"
    catalogue["expires"] = "2027-11-02T00:00:00Z"
    catalogue["packages"] = [
        {
            "kind": "expansion",
            "id": "boneyards",
            "name": "The Boneyards",
            "version": "1",
            "release": 1,
            "size": package.stat().st_size,
            "sha256": digest,
            "file": "/v1/p/" + digest + ".oaexp",
            "extends": ["totala"],
        }
    ]
    catalogue_path.write_text(json.dumps(catalogue) + "\n", encoding="utf-8")
    filed = serve_dir / "v1" / "p" / (digest + ".oaexp")
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


def publish_feed(tool, serve_dir, key, passphrase, port):
    """Signs a feed of version 99.0.0 and returns its address and public key."""
    files = serve_dir / "files"
    files.mkdir(parents=True, exist_ok=True)
    packages = []
    for platform in PLATFORMS:
        package = files / f"open-annihilation-99.0.0-{platform}.zip"
        package.write_bytes(b"x")
        packages.append(str(package))
    base = f"http://127.0.0.1:{port}"
    feed = serve_dir / "feed.json"
    require(
        run(
            [
                tool,
                "engine-feed",
                "write",
                "--version=99.0.0",
                "--date",
                "2026-11-02",
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
    made = require(
        run(
            [
                tool,
                "catalogue",
                "public",
                str(key),
            ]
        ),
        "catalogue public",
    )
    return base + "/feed.json", public_line(made)


def run_game(game, game_tree, work, mode, extra):
    """Starts the entry check and returns whether its output matches."""
    user = work / "user"
    data = work / "data"
    user.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    prefs = work / "prefs.conf"
    preferences(prefs)
    frames = "9600" if mode == "install" else "2400"
    command = [
        game,
        "--game-dir",
        str(game_tree),
        "--skip-intro",
        "--mute",
        "--check-boneyards-entry",
        mode,
        "--frames",
        frames,
        "--user-folder",
        str(user),
        "--data-dir",
        str(data),
        "--preferences-file",
        str(prefs),
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
        return fail(f"the game exited {completed.returncode}")
    output = completed.stdout
    if mode == "install":
        archive = user / "Expansions" / "totala" / "boneyards" / "by.ccx"
        if "entry flow: boneyards open" not in output:
            return fail("the Boneyards did not open")
        if "entry flow: failed" in output:
            return fail("the entry failed")
        if not archive.is_file():
            return fail("the expansion archive was not installed")
        if "boneyards" not in prefs.read_text(encoding="utf-8"):
            return fail("the expansion was not turned on")
        return 0
    if "entry flow: asking to update" not in output or QUESTION not in output:
        return fail("the update question was not asked")
    if "entry flow: stayed" not in output:
        return fail("Not now did not stay on the menu")
    if "entry flow: boneyards open" in output:
        return fail("Not now opened the Boneyards")
    return 0


def prepare(tool, work, game_dir, mode):
    """Builds the fixture and returns the game tree, the server and extra arguments."""
    serve_dir = work / "serve"
    serve_dir.mkdir(parents=True)
    server = serve(serve_dir)
    port = server.server_address[1]
    base = f"http://127.0.0.1:{port}"
    passphrase = work / "passphrase"
    passphrase.write_text(PASSPHRASE + "\n", encoding="utf-8")
    extra = []
    if mode == "install":
        repo = Path(__file__).resolve().parents[4]
        folder = work / "expansion"
        stage_expansion(game_dir, folder, bundle_names(repo))
        packed = work / "boneyards.oaexp"
        require(run([tool, "pack", str(folder), "--out", str(packed), "--force"]), "pack")
        key = work / "registry.key"
        made = require(
            run(
                [
                    tool,
                    "catalogue",
                    "keygen",
                    "--id",
                    REGISTRY_ID,
                    "--out",
                    str(key),
                    "--passphrase-file",
                    str(passphrase),
                ]
            ),
            "catalogue keygen",
        )
        require(
            run(
                [
                    tool,
                    "registry",
                    "init",
                    str(serve_dir),
                    "--id",
                    REGISTRY_ID,
                    "--name",
                    "E01 Local",
                    "--base-url",
                    base,
                    "--key",
                    str(key),
                    "--passphrase-file",
                    str(passphrase),
                ]
            ),
            "registry init",
        )
        publish_catalogue(tool, serve_dir, key, passphrase, packed)
        user = work / "user"
        user.mkdir(parents=True, exist_ok=True)
        (user / "Registries.yaml").write_text(
            registries_text(base, public_line(made)), encoding="utf-8"
        )
        game_tree = work / "game"
        link_tree(game_dir, game_tree, {"by.ccx"})
    else:
        key = work / "feed.key"
        require(
            run(
                [
                    tool,
                    "catalogue",
                    "keygen",
                    "--id",
                    FEED_ID,
                    "--out",
                    str(key),
                    "--passphrase-file",
                    str(passphrase),
                ]
            ),
            "feed keygen",
        )
        feed_url, public = publish_feed(tool, serve_dir, key, passphrase, port)
        install = work / "update-install"
        install.mkdir()
        extra = [
            "--update-feed",
            feed_url,
            "--update-key",
            FEED_ID + "=" + public,
            "--update-install-folder",
            str(install),
        ]
        user = work / "user"
        user.mkdir(parents=True, exist_ok=True)
        (user / "Registries.yaml").write_text(
            'disabled:\n  - "coreprime"\n', encoding="utf-8"
        )
        game_tree = work / "game"
        link_tree(game_dir, game_tree, set())
    return game_tree, server, extra


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--game-dir", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("install", "update"))
    parser.add_argument("--work", required=True, type=Path)
    args = parser.parse_args()
    if args.work.exists():
        shutil.rmtree(args.work)
    args.work.mkdir(parents=True)
    server = None
    try:
        game_tree, server, extra = prepare(args.tool, args.work, args.game_dir, args.mode)
        return run_game(args.game, game_tree, args.work, args.mode, extra)
    except RuntimeError as error:
        return fail(str(error))
    finally:
        if server is not None:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    sys.exit(main())
