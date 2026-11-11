#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One native check. A loopback registry lists Ridge, an old map pack and a
# mutator. The game fetches the pack and the mutator the replay names, plays,
# and puts the player's mutators back. With Ridge 2.9 installed it fetches
# 2.8 beside that release, plays, and puts 2.9 back. That copy is then moved
# under Mods and played from there. An update of Ridge in the same run changes
# what the row offers. A downloaded replay whose map is only in a kept pack is
# read by the playback bar with that pack. A .tad of a Ridge game, with the
# content note 0.8 wrote naming Ridge, plays and is read with the installed
# Ridge, and once Ridge is gone one press fetches the catalogue's Ridge and
# plays it. A read refused under an older build is tried again, and one
# refused under this build is not. Nothing is fetched from a live server.

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import native_offline

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
        self.stopped = False


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

        def _refuse_if_stopped(self):
            """Closes the connection once the check has asked the registry to stop."""
            with state.lock:
                stopped = state.stopped
            if not stopped:
                return False
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.close_connection = True
            return True

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/stop":
                with state.lock:
                    state.stopped = True
                self._send(204, b"", "text/plain")
                return
            if self._refuse_if_stopped():
                return
            relative = path.lstrip("/")
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
            if self._refuse_if_stopped():
                return
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
            and "Ridge Replays's catalogue" in played.stdout
            and "replays content: 409 left no Watch" in played.stdout
            and "Get Ridge 2.8 and watch" in played.stdout
            and "Getting Ridge Reach" in played.stdout
            and "Getting ridge-reach" not in played.stdout
            and "replays content: ridge playing" in played.stdout
            and "replays content: ridge again" in played.stdout
            and played.stdout.count("replays content: end screen stayed until Main Menu") >= 2
            and played.stdout.count("replays content: library open on default/ridge.oademo") >= 2
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

        with state.lock:
            posted_before = len(state.posts)
        ridge_new_sha = sha256_file(work / "ridge-29.oamod")
        write_facts(
            facts,
            {
                "case": "beside",
                "descriptor": state.base + "/registry.yaml",
                "registry": REGISTRY_ID,
                "pack_sha": old_sha,
                "mutator_sha": mutator_sha,
                "ridge_sha": ridge_sha,
                "ridge_new_sha": ridge_new_sha,
                "unlisted_sha": unlisted,
                "gone_sha": gone_sha,
            },
        )
        # No --snapshot: the play case already wrote the card pictures.
        beside = run_game(
            args.native,
            args.game_dir,
            work,
            work / "beside",
            ["--check-replays-content"],
        )
        with state.lock:
            fresh = list(state.posts[posted_before:])
        beside_parsed = []
        for body in fresh:
            try:
                beside_parsed.append(json.loads(body))
            except json.JSONDecodeError:
                beside_parsed.append({})
        asked_ridge = any(item.get("sha256") == ridge_sha for item in beside_parsed)
        asked_ridge_count = sum(1 for item in beside_parsed if item.get("sha256") == ridge_sha)
        asked_newer = any(item.get("sha256") == ridge_new_sha for item in beside_parsed)
        beside_tables = [
            int(count)
            for count in re.findall(
                r"^demo unit table: (\d+) recorded, \1 matched, 0 missing, 0 extra here$",
                beside.stdout,
                re.MULTILINE,
            )
        ]
        beside_ok = (
            beside.returncode == 0
            and "Get Ridge 2.8 and watch" in beside.stdout
            and "You have 2.9" in beside.stdout
            and "replays content: ridge 2.8 playing" in beside.stdout
            and "replays content: 2.9 chosen" in beside.stdout
            and "replays content: end screen stayed until Main Menu" in beside.stdout
            and "replays content: library open on default/ridge.oademo" in beside.stdout
            and "a stop during the replay chose Ridge 2.9" in beside.stdout
            and "replays content: the next start played nothing" in beside.stdout
            and 'button "Watch"' in beside.stdout
            and "You're offline" not in beside.stdout
            and "replays content: kept ridge 2.8 playing" in beside.stdout
            and "replays content: 2.9 chosen again" in beside.stdout
            and "replays content: alongside ridge 2.8 playing" in beside.stdout
            and len(beside_tables) >= 2
            and asked_ridge
            and asked_ridge_count == 1
            and not asked_newer
        )
        if not beside_ok:
            print(
                f"beside failed: status {beside.returncode}, "
                f"ridge {asked_ridge} x{asked_ridge_count}, newer {asked_newer}, "
                f"tables {beside_tables}, "
                f"posts {fresh}",
                file=sys.stderr,
            )
            return 1

        # The beside case asks the registry to stop. This case needs it again.
        with state.lock:
            state.stopped = False
        write_facts(
            facts,
            {
                "case": "update",
                "descriptor": state.base + "/registry.yaml",
                "registry": REGISTRY_ID,
                "pack_sha": old_sha,
                "mutator_sha": mutator_sha,
                "ridge_sha": ridge_sha,
                "unlisted_sha": unlisted,
                "gone_sha": gone_sha,
            },
        )
        updated = run_game(
            args.native,
            args.game_dir,
            work,
            work / "update",
            ["--check-replays-content"],
        )
        update_ok = (
            updated.returncode == 0
            and 'button "Watch"' in updated.stdout
            and "installed in" in updated.stdout
            and "Mods/ridge" in updated.stdout
            and "Get Ridge 2.8 and watch" in updated.stdout
            and "You have 2.9." in updated.stdout
            and "replays content: update noticed" in updated.stdout
        )
        if not update_ok:
            print(f"update failed: status {updated.returncode}", file=sys.stderr)
            print("\n".join(updated.stdout.splitlines()[-30:]), file=sys.stderr)
            return 1

        with state.lock:
            posted_before = len(state.posts)
        write_facts(
            facts,
            {
                "case": "packs",
                "descriptor": state.base + "/registry.yaml",
                "registry": REGISTRY_ID,
                "pack_sha": old_sha,
                "pack_new_sha": new_sha,
                "pack_new_release": str(NEW_PACK_RELEASE),
                "mutator_sha": mutator_sha,
                "ridge_sha": ridge_sha,
                "ridge_new_sha": sha256_file(work / "ridge-29.oamod"),
                "unlisted_sha": unlisted,
                "gone_sha": gone_sha,
            },
        )
        packs = run_game(
            args.native,
            args.game_dir,
            work,
            work / "packs",
            ["--check-replays-content"],
        )
        with state.lock:
            fresh = list(state.posts[posted_before:])
        packs_parsed = []
        for body in fresh:
            try:
                packs_parsed.append(json.loads(body))
            except json.JSONDecodeError:
                packs_parsed.append({})
        asked_old = [
            item for item in packs_parsed if item.get("sha256") == old_sha
        ]
        asked_new = any(item.get("sha256") == new_sha for item in packs_parsed)
        asked_ridge = sum(1 for item in packs_parsed if item.get("sha256") == ridge_sha)
        ridge_new_sha = sha256_file(work / "ridge-29.oamod")
        asked_ridge_new = any(item.get("sha256") == ridge_new_sha for item in packs_parsed)
        packs_ok = (
            packs.returncode == 0
            and "replays content: hand played with no fetch" in packs.stdout
            and "another release of the map pack Ridge Reach" in packs.stdout
            and "replays content: older pack playing from Replay releases" in packs.stdout
            and "replays content: installed pack unchanged" in packs.stdout
            and "replays content: greyed pack played from Replay releases" in packs.stdout
            and "index pass read default/older.oademo" in packs.stdout
            and "replays content: kept pack played with nothing in Maps" in packs.stdout
            and "replays content: file origin watched with no fetch" in packs.stdout
            and "replays content: older pack and older mod playing" in packs.stdout
            and "replays content: 2.9 chosen after the pack" in packs.stdout
            and "map pack ridge-reach: mounted" in packs.stdout
            and "replays content: bar read Core Prime/Ridge Reach Download.oademo with the kept pack"
            in packs.stdout
            and "demo map 'ridge_reach@ridge-reach' is not installed" not in packs.stdout
            and len(asked_old) == 1
            and asked_ridge == 1
            and not asked_new
            and not asked_ridge_new
        )
        if not packs_ok:
            print(
                f"packs failed: status {packs.returncode}, "
                f"old {len(asked_old)}, ridge {asked_ridge}, newer {asked_new}, "
                f"ridge 2.9 {asked_ridge_new}, posts {fresh}",
                file=sys.stderr,
            )
            print("\n".join(packs.stdout.splitlines()[-40:]), file=sys.stderr)
            return 1

        with state.lock:
            posted_before = len(state.posts)
        write_facts(
            facts,
            {
                "case": "local",
                "descriptor": state.base + "/registry.yaml",
                "registry": REGISTRY_ID,
                "pack_sha": old_sha,
                "mutator_sha": mutator_sha,
                "ridge_sha": ridge_sha,
                "unlisted_sha": unlisted,
                "gone_sha": gone_sha,
            },
        )
        local = run_game(
            args.native,
            args.game_dir,
            work,
            work / "local",
            ["--check-replays-content"],
        )
        with state.lock:
            local_posts = list(state.posts[posted_before:])
        local_parsed = []
        for body in local_posts:
            try:
                local_parsed.append(json.loads(body))
            except json.JSONDecodeError:
                local_parsed.append({})
        ridge_new_sha = sha256_file(work / "ridge-29.oamod")
        asked_local = [item.get("sha256") for item in local_parsed]
        local_tables = re.findall(
            r"^demo unit table: (\d+) recorded, \1 matched, 0 missing, 0 extra here$",
            local.stdout,
            re.MULTILINE,
        )
        local_ok = (
            local.returncode == 0
            and "replays content: local .tad files written" in local.stdout
            and local.stdout.count("replays content: index pass read ridge/") == 3
            and "replays content: local tad playing with Ridge" in local.stdout
            and "replays content: bar read ridge/2026-10-20 0754 Named Ridge.tad with Ridge"
            in local.stdout
            and "replays content: no mod after the local tad" in local.stdout
            and "replays content: local tad needs Ridge once it is gone" in local.stdout
            and "replays content: local tad playing with the fetched Ridge 2.9" in local.stdout
            and "replays content: no mod after the fetched Ridge" in local.stdout
            and "replays content: a read refused under an older build was read" in local.stdout
            and "replays content: a read refused under this build stayed refused" in local.stdout
            and "index pass could not read" not in local.stdout
            and "needs the recording's unit definitions" not in local.stdout
            and len(local_tables) >= 2
            and asked_local == [ridge_new_sha]
        )
        if not local_ok:
            print(
                f"local failed: status {local.returncode}, tables {len(local_tables)}, "
                f"posts {local_posts}",
                file=sys.stderr,
            )
            print("\n".join(local.stdout.splitlines()[-40:]), file=sys.stderr)
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
        and "replays content: end screen stayed until Main Menu" in expansion.stdout
        and "0 missing, 0 extra" in expansion.stdout
        and "replays content: expansions restored" in expansion.stdout
        and "replays content: library open on default/expansion.oademo" in expansion.stdout
        and "replays content: the next start played nothing" in expansion.stdout
        and "a stop during the replay put the set back" in expansion.stdout
        and "a start that ended before the replay put the set back" in expansion.stdout
        and "Watched to " in expansion.stdout
        and "replays content: watch from reached tick " in expansion.stdout
        and "replays content: the switched replay put the set back" in expansion.stdout
        and "replays content: a replay played to its last tick keeps no place" in expansion.stdout
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
