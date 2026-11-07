#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One native check. A loopback registry lists Ridge, an old map pack and a
# mutator. The game fetches the pack and the mutator the replay names, plays,
# and puts the player's mutators back. Nothing is fetched from a live server.

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REGISTRY_ID = "ridge-replays"
KEY_ID = "ridge-replays"
PASSPHRASE = "r12-local-pass"
PACK_RELEASE = 2
NEW_PACK_RELEASE = 9
MUTATOR_RELEASE = 4
RIDGE_OLD_RELEASE = 1
RIDGE_NEW_RELEASE = 3


def fail(message):
    """Prints a failure and returns the failed status."""
    print(message, file=sys.stderr)
    return 1


def run(command, cwd):
    """Runs one command and keeps its output. No shell is used."""
    completed = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed


def require_command(completed, what):
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


def preferences(path):
    """A finished welcome, in English, that checks for no catalogue or update."""
    path.write_text(
        "open-annihilation-preferences 1\n"
        '"open-annihilation.language" "en"\n'
        '"open-annihilation.first-run" "done"\n'
        '"open-annihilation.content-updates" "never"\n'
        '"open-annihilation.update-check" "never"\n',
        encoding="utf-8",
    )


def disabled_registries(path):
    """Turns the built-in registry off, so the check never calls it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        'registries: []\ndisabled:\n  - "coreprime"\n',
        encoding="utf-8",
    )


def sha256_file(path):
    """The lower-case SHA-256 of a file."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def package(kind, package_id, name, release, path, version=None, revision=None, maps=None):
    """One catalogue or releases entry for a file."""
    digest = sha256_file(path)
    entry = {
        "kind": kind,
        "id": package_id,
        "name": name,
        "release": release,
        "size": path.stat().st_size,
        "sha256": digest,
        "file": f"/v1/p/{digest}.{kind}",
    }
    if version is not None:
        entry["version"] = version
    if revision is not None:
        entry["revision"] = revision
    if maps is not None:
        entry["maps"] = maps
    if kind == "oamut":
        entry["works_with"] = ["*"]
        entry["layers"] = ["files"]
    return entry


class RegistryState:
    """What the loopback registry serves, and the key requests it saw."""

    def __init__(self):
        self.directory = None
        self.base = ""
        self.files = {}
        self.posts = []
        self.lock = threading.Lock()
        self.next_id = 1


def make_handler(state):
    """A handler for one registry. It writes nothing of its own."""

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            return

        def _send(self, status, body, content_type):
            data = body if isinstance(body, bytes) else body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            relative = self.path.split("?", 1)[0].lstrip("/")
            target = (state.directory / relative).resolve()
            root = state.directory.resolve()
            if not str(target).startswith(str(root)) or not target.is_file():
                self._send(404, b"", "text/plain")
                return
            kind = "application/json" if target.suffix == ".json" else "application/octet-stream"
            if target.suffix == ".sig":
                kind = "text/plain"
            # A package takes a moment, so the card can show the fetch.
            if relative.startswith("v1/p/"):
                time.sleep(0.4)
            self._send(200, target.read_bytes(), kind)

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            path = self.path.split("?", 1)[0]
            if path.startswith("/api/v1/downloads/") and path.endswith("/result"):
                self._send(204, b"", "text/plain")
                return
            if path != "/api/v1/downloads":
                self._send(404, b"", "text/plain")
                return
            with state.lock:
                state.posts.append(body.decode("utf-8", errors="replace"))
                download_id = f"d{state.next_id}"
                state.next_id += 1
            try:
                payload = json.loads(body.decode("utf-8"))
            except json.JSONDecodeError:
                self._send(400, '{"error":"bad_request"}', "application/json")
                return
            digest = payload.get("sha256", "")
            kind = state.files.get(digest)
            if kind is None:
                self._send(
                    409,
                    '{"error":"release_mismatch","message":"no longer listed"}',
                    "application/json",
                )
                return
            answer = {
                "download": download_id,
                "expires": "2027-06-01T00:00:00Z",
                "url": f"{state.base}/v1/p/{digest}.{kind}",
            }
            self._send(201, json.dumps(answer), "application/json")

    return Handler


def serve(directory, state):
    """Serves directory on 127.0.0.1 and returns the server."""
    state.directory = directory
    server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(state))
    port = server.server_address[1]
    state.base = f"http://127.0.0.1:{port}"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def write_facts(path, values):
    """Writes the facts the game reads. One `key=value` a line."""
    lines = [f"{key}={values[key]}" for key in values]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def game_env():
    """The environment one start uses. SDL stays headless."""
    env = os.environ.copy()
    env["SDL_VIDEO_DRIVER"] = "dummy"
    env["SDL_AUDIO_DRIVER"] = "dummy"
    env["SDL_RENDER_DRIVER"] = "software"
    return env


def run_game(native, game_dir, work, case_dir, extra):
    """Runs one start. Its output is printed. The status is returned."""
    user = case_dir / "user"
    data = case_dir / "data"
    if user.exists():
        shutil.rmtree(user)
    if data.exists():
        shutil.rmtree(data)
    user.mkdir(parents=True)
    data.mkdir(parents=True)
    prefs = case_dir / "prefs.conf"
    preferences(prefs)
    disabled_registries(user / "Registries.yaml")
    command = [
        native,
        "--game-dir",
        game_dir,
        "--skip-intro",
        "--mute",
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
        cwd=str(work),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=game_env(),
    )
    sys.stdout.write(completed.stdout)
    sys.stdout.flush()
    return completed


def publish(tool, work):
    """Builds the catalogue and the releases list and signs the catalogue."""
    old_pack = work / "ridge-reach.oamap"
    new_pack = work / "ridge-reach-new.oamap"
    mutator = work / "ridge-wind.oamut"
    ridge_old = work / "ridge-28.oamod"
    ridge_new = work / "ridge-29.oamod"
    gone = work / "ridge-gone.oamap"
    gone.write_bytes(b"gone\n")
    for path in (old_pack, new_pack, mutator, ridge_old, ridge_new, gone):
        if not path.is_file():
            raise RuntimeError(path.name + " was not written")
    maps = [
        {
            "map": "ridge_reach@ridge-reach",
            "name": "Ridge Reach",
            "players": 2,
            "size": "4x4",
        }
    ]
    old_entry = package("oamap", "ridge-reach", "Ridge Reach", PACK_RELEASE, old_pack, "1.0", maps=maps)
    new_entry = package(
        "oamap", "ridge-reach", "Ridge Reach", NEW_PACK_RELEASE, new_pack, "1.0", maps=maps
    )
    mutator_entry = package("oamut", "ridge-wind", "Ridge Wind", MUTATOR_RELEASE, mutator, "1")
    ridge_old_entry = package("oamod", "ridge", "Ridge", RIDGE_OLD_RELEASE, ridge_old, "2.8", 1)
    ridge_new_entry = package("oamod", "ridge", "Ridge", RIDGE_NEW_RELEASE, ridge_new, "2.9", 1)
    gone_maps = [
        {
            "map": "ridge_gone@ridge-gone",
            "name": "Ridge Gone",
            "players": 2,
            "size": "4x4",
        }
    ]
    gone_entry = package("oamap", "ridge-gone", "Ridge Gone", 1, gone, "1.0", maps=gone_maps)
    releases = {
        "releases": 1,
        "registry": REGISTRY_ID,
        "sequence": 60,
        "generated": "2026-11-01T00:00:00Z",
        "packages": [old_entry, ridge_old_entry],
    }
    releases_bytes = (json.dumps(releases, separators=(",", ":")) + "\n").encode("utf-8")
    releases_sha = hashlib.sha256(releases_bytes).hexdigest()
    serve_dir = work / "serve"
    if serve_dir.exists():
        shutil.rmtree(serve_dir)
    releases_path = serve_dir / "v1" / "releases" / f"{releases_sha}.json"
    releases_path.parent.mkdir(parents=True)
    releases_path.write_bytes(releases_bytes)
    key = work / "ridge.key"
    passphrase = work / "pass.txt"
    if key.exists():
        key.unlink()
    passphrase.write_text(PASSPHRASE + "\n", encoding="utf-8")
    made = require_command(
        run(
            [
                tool,
                "catalogue",
                "keygen",
                "--id",
                KEY_ID,
                "--out",
                str(key),
                "--passphrase-file",
                str(passphrase),
            ],
            work,
        ),
        "catalogue keygen",
    )
    public = public_line(made)
    catalogue = {
        "catalogue": 1,
        "registry": REGISTRY_ID,
        "sequence": 60,
        "generated": "2026-11-01T00:00:00Z",
        "expires": "2027-11-01T00:00:00Z",
        "keys": [{"id": KEY_ID, "public": public}],
        "releases": {
            "file": f"/v1/releases/{releases_sha}.json",
            "sha256": releases_sha,
            "size": len(releases_bytes),
        },
        "packages": [new_entry, mutator_entry, ridge_new_entry, gone_entry],
    }
    catalogue_path = serve_dir / "v1" / "catalogue.json"
    catalogue_path.write_text(json.dumps(catalogue, separators=(",", ":")) + "\n", encoding="utf-8")
    filed = serve_dir / "v1" / "p"
    filed.mkdir()
    for entry, path in (
        (old_entry, old_pack),
        (new_entry, new_pack),
        (mutator_entry, mutator),
        (ridge_old_entry, ridge_old),
        (ridge_new_entry, ridge_new),
    ):
        shutil.copy2(path, filed / f"{entry['sha256']}.{entry['kind']}")
    require_command(
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
            ],
            work,
        ),
        "catalogue sign",
    )
    state = RegistryState()
    state.files = {
        old_entry["sha256"]: "oamap",
        new_entry["sha256"]: "oamap",
        mutator_entry["sha256"]: "oamut",
        ridge_old_entry["sha256"]: "oamod",
        ridge_new_entry["sha256"]: "oamod",
    }
    return (
        state,
        serve_dir,
        old_entry["sha256"],
        new_entry["sha256"],
        mutator_entry["sha256"],
        ridge_old_entry["sha256"],
        gone_entry["sha256"],
        public,
    )


def registry_text(base, public):
    """A tokens registry served on this machine."""
    return (
        "registry: 1\n"
        f'id: "{REGISTRY_ID}"\n'
        'name: "Ridge Replays"\n'
        f'catalogue: "{base}/v1/catalogue.json"\n'
        "keys:\n"
        f'  - id: "{KEY_ID}"\n'
        f'    public: "{public}"\n'
        "downloads:\n"
        "  mode: tokens\n"
        f'  api: "{base}/api/v1"\n'
        "  install-id: none\n"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    source = work / "replays-content-source.oademo"
    record_dir = work / "record"
    recorded = run_game(
        args.native,
        args.game_dir,
        work,
        record_dir,
        [
            "--headless-check",
            "--net-loopback-check",
            "300",
            "--record-game",
            str(source),
        ],
    )
    if recorded.returncode != 0 or not source.is_file():
        return fail("the recorded game was not written")

    facts = work / "replays-content.facts"
    write_facts(facts, {"case": "prepare"})
    prepared = run_game(
        args.native,
        args.game_dir,
        work,
        work / "prepare",
        ["--check-replays-content"],
    )
    if prepared.returncode != 0 or "replays content: packages written" not in prepared.stdout:
        return fail("the packages were not written")

    ridge_source = work / "replays-content-ridge.oademo"
    recorded_ridge = run_game(
        args.native,
        args.game_dir,
        work,
        work / "ridge-record",
        [
            "--headless-check",
            "--net-loopback-check",
            "300",
            "--record-game",
            str(ridge_source),
            "--mod-dir",
            str(work / "ridge-mod"),
        ],
    )
    if recorded_ridge.returncode != 0 or not ridge_source.is_file():
        return fail("the Ridge recording was not written")

    state, serve_dir, old_sha, new_sha, mutator_sha, ridge_sha, gone_sha, public = publish(
        args.tool, work
    )
    server = serve(serve_dir, state)
    try:
        (serve_dir / "registry.yaml").write_text(
            registry_text(state.base, public), encoding="utf-8"
        )
        unlisted = "b" * 64
        write_facts(
            facts,
            {
                "case": "play",
                "descriptor": state.base + "/registry.yaml",
                "registry": REGISTRY_ID,
                "pack_sha": old_sha,
                "mutator_sha": mutator_sha,
                "ridge_sha": ridge_sha,
                "unlisted_sha": unlisted,
                "gone_sha": gone_sha,
            },
        )
        pictures = work / "pictures"
        pictures.mkdir(exist_ok=True)
        played = run_game(
            args.native,
            args.game_dir,
            work,
            work / "play",
            [
                "--check-replays-content",
                "--snapshot",
                str(pictures / "replays.ppm"),
            ],
        )
        with state.lock:
            posts = list(state.posts)
        parsed = []
        for body in posts:
            try:
                parsed.append(json.loads(body))
            except json.JSONDecodeError:
                parsed.append({})
        named_old = any(
            item.get("sha256") == old_sha and item.get("release") == PACK_RELEASE for item in parsed
        )
        named_mutator = any(item.get("sha256") == mutator_sha for item in parsed)
        named_new = any(item.get("sha256") == new_sha for item in parsed)
        named_gone = any(item.get("sha256") == gone_sha for item in parsed)
        tables = [
            int(count)
            for count in re.findall(
                r"^demo unit table: (\d+) recorded, \1 matched, 0 missing, 0 extra here$",
                played.stdout,
                re.MULTILINE,
            )
        ]
        play_ok = (
            played.returncode == 0
            and "Get them and watch" in played.stdout
            and "Getting" in played.stdout
            and "replays content: watching" in played.stdout
            and "demo mutators: 1 applied" in played.stdout
            and "replays content: mutators selected 0" in played.stdout
            and "not in Core Prime's catalogue" in played.stdout
            and "replays content: 409 left no Watch" in played.stdout
            and "Get Ridge 2.8 and watch" in played.stdout
            and "Getting Ridge Reach" in played.stdout
            and "Getting ridge-reach" not in played.stdout
            and "replays content: ridge playing" in played.stdout
            and "replays content: ridge again" in played.stdout
            and "replays content: the next start played nothing" in played.stdout
            and "replays content: no mod" in played.stdout
            and "a stop during the replay chose No Mod" in played.stdout
            and len(tables) >= 3
            and tables[1] == tables[0] + 1
            and tables[2] == tables[1]
            and named_old
            and named_mutator
            and named_gone
            and not named_new
        )
        if not play_ok:
            print(
                f"play failed: status {played.returncode}, "
                f"old {named_old}, mutator {named_mutator}, newer {named_new}, "
                f"gone {named_gone}, tables {tables}, posts {posts}",
                file=sys.stderr,
            )
            return 1

        play_yaml = work / "play" / "user" / "Registries.yaml"
    finally:
        server.shutdown()
        server.server_close()

    # The server is stopped. Each offline start has its own folders; run_game
    # would wipe the registry file just copied, so these starts are their own.
    # The play case's data folder keeps the catalogue cache.
    def offline_start(case_dir, values):
        """One offline start from a copy of the play case's cache."""
        case_user = case_dir / "user"
        case_data = case_dir / "data"
        if case_dir.exists():
            shutil.rmtree(case_dir)
        case_user.mkdir(parents=True)
        shutil.copy2(play_yaml, case_user / "Registries.yaml")
        shutil.copytree(work / "play" / "data", case_data)
        # The catalogue and the releases list stay. The packages the play
        # case already fetched must not: a press would install those files
        # and never ask the registry, so it would not say the player is offline.
        downloads = case_data / "content" / "downloads"
        if downloads.exists():
            shutil.rmtree(downloads)
        case_prefs = case_dir / "prefs.conf"
        preferences(case_prefs)
        write_facts(facts, values)
        command = [
            args.native,
            "--game-dir",
            args.game_dir,
            "--skip-intro",
            "--mute",
            "--check-replays-content",
            "--snapshot",
            str(pictures / "replays.ppm"),
            "--user-folder",
            str(case_user),
            "--data-dir",
            str(case_data),
            "--preferences-file",
            str(case_prefs),
        ]
        return subprocess.run(
            command,
            cwd=str(work),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=game_env(),
        )

    shared = {
        "descriptor": state.base + "/registry.yaml",
        "registry": REGISTRY_ID,
        "pack_sha": old_sha,
        "mutator_sha": mutator_sha,
        "ridge_sha": ridge_sha,
        "unlisted_sha": unlisted,
        "gone_sha": gone_sha,
    }
    cached = offline_start(work / "offline-cached", {"case": "offline", "cached": "press", **shared})
    sys.stdout.write(cached.stdout)
    sys.stdout.flush()
    if (
        cached.returncode != 0
        or "You're offline." not in cached.stdout
        or "offline after a press" not in cached.stdout
        or "Ridge Wind" not in cached.stdout
        or "Ridge Reach" not in cached.stdout
    ):
        return fail(f"cached offline failed: status {cached.returncode}")

    offline = offline_start(work / "offline", {"case": "offline", **shared})
    sys.stdout.write(offline.stdout)
    sys.stdout.flush()
    if (
        offline.returncode != 0
        or "You're offline." not in offline.stdout
        or "Ridge Wind" not in offline.stdout
        or "Ridge Reach" not in offline.stdout
        or "Get ridge-wind" in offline.stdout
    ):
        return fail(f"offline failed: status {offline.returncode}")

    write_facts(facts, {"case": "expansion"})
    expansion = run_game(
        args.native,
        args.game_dir,
        work,
        work / "expansion",
        ["--check-replays-content"],
    )
    expansion_ok = (
        expansion.returncode == 0
        and "Turn on and watch" in expansion.stdout
        and "replays content: expansion playing" in expansion.stdout
        and "0 missing, 0 extra" in expansion.stdout
        and "replays content: expansions restored" in expansion.stdout
        and "replays content: the next start played nothing" in expansion.stdout
        and "a stop during the replay put the set back" in expansion.stdout
        and "a start that ended before the replay put the set back" in expansion.stdout
    )
    if not expansion_ok:
        return fail(f"expansion failed: status {expansion.returncode}")
    print("replays content: check passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"replays content: {error}", file=sys.stderr)
        sys.exit(1)
