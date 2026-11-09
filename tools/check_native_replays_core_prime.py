#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# One native check. A loopback server on 127.0.0.1 is Core Prime's replay
# feed and its download-key server. The game lists a page, asks for the next
# one, filters by planet, downloads a replay, reads a release that changed,
# refuses a file whose digest differs, and watches the replay it saved. A
# second start reads the list with that server stopped. A share plants an
# address sector and a chat line; the server reads the uploaded replay and
# checks the sector is gone and the chat is there. Selecting a recording of
# a registered kind asks the feed, which answers not listed and then listed.
# No request leaves 127.0.0.1.

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

import check_native_replays_content as ridge_registry

PAGE_SIZE = 48
ITEM_COUNT = 50
REAL_PLANET = "Ashmere"
OTHER_PLANET = "Cinder"
SECTOR = "North Span"
KIND = "outpost"
WATCH_TITLE = "North Reach"
MISMATCH_TITLE = "West Reach"
BAD_TITLE = "Bad Reach"
FAKE_SHA = "cd" * 32
BAD_SHA = "ab" * 32
BAD_BYTES = b"not-the-replay"
TAD_SHA = "ee" * 32
# Planted in the shared recording. The upload must keep the chat and drop the address.
CHAT_MARK = b"R20CHATMARK says hello"
ADDRESS_TEXT = b"203.0.113.77"
LOOKUP_GAME = "lookup-1"
LOOKUP_ID = "r_lookup0000000000000001"


def fail(message):
    """Prints a failure and returns the failed status."""
    print(message, file=sys.stderr)
    return 1


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


def replay_id(index):
    """One feed id: r_ and 22 digits."""
    return f"r_{index:022d}"


class FeedState:
    """What the loopback server serves, and the requests it saw."""

    def __init__(self, recorded):
        self.recorded = recorded
        self.real_sha = hashlib.sha256(recorded).hexdigest()
        self.base = ""
        self.queries = []
        self.entry_gets = []
        self.keys = []
        self.results = []
        self.tad = False
        self.bumped = False
        self.next_download = 1
        self.mode = "play"
        self.query_times = []
        self.rate_sent = 0
        self.challenge_solved = False
        self.polled = False
        self.beside_bytes = b""
        self.beside_mod_sha = ""
        self.uploads = []
        self.upload_auths = []
        self.logins = []
        self.battle_queries = []
        self.lookup_game = LOOKUP_GAME
        self.lock = threading.Lock()


def title_of(index):
    """The map name the list shows for one place."""
    if index == 0:
        return WATCH_TITLE
    if index == 1:
        return MISMATCH_TITLE
    if index == 2:
        return BAD_TITLE
    planet = REAL_PLANET if index < 30 else OTHER_PLANET
    return f"{planet} {index}"


def entry_of(state, index, detail):
    """One feed entry. A detail adds an empty needs list."""
    if index == 0 and state.mode == "beside" and state.beside_bytes:
        revision = 3
        digest = hashlib.sha256(state.beside_bytes).hexdigest()
        size = len(state.beside_bytes)
    elif index == 0:
        revision = 3
        digest = state.real_sha
        size = len(state.recorded)
    elif index == 1 and state.bumped:
        revision = 2
        digest = state.real_sha
        size = len(state.recorded)
    elif index == 1:
        revision = 1
        digest = FAKE_SHA
        size = len(state.recorded)
    elif index == 2:
        revision = 1
        digest = BAD_SHA
        size = len(BAD_BYTES)
    else:
        revision = 1
        digest = state.real_sha
        size = len(state.recorded)
    planet = REAL_PLANET if index < 30 else OTHER_PLANET
    sector = SECTOR if index == 0 else "South Span"
    entry = {
        "id": replay_id(index),
        "revision": revision,
        "title": title_of(index),
        "source": "capture",
        "visibility": "public",
        "recorded_at": "2026-11-04T14:32:10Z",
        "length_ms": 60000 + index * 1000,
        "game": {
            "kind": KIND,
            "planet": planet,
            "sector": sector,
            "spare": True,
        },
        "map": {"name": title_of(index)},
        "players": [
            {
                "name": "Pilot",
                "side": "arm",
                "colour": 0,
                "team": 1,
                "result": "won",
                "kills": 41 if index == 0 else 1,
            }
        ],
        "mod": None,
        "uploader": "",
        "downloads": ITEM_COUNT - index,
        "files": {
            "oademo": {"sha256": digest, "size": size},
            "tad": {"sha256": TAD_SHA, "size": 12},
        },
        "beacon": True,
    }
    # The title is not a map. A map need for it would sit beside the mod and
    # change the button. The recording's own map is what the replay plays.
    if state.mode == "beside" and index == 0:
        entry.pop("map", None)
    if detail and state.mode == "beside" and index == 0 and state.beside_mod_sha:
        entry["mod"] = {
            "id": "ridge",
            "name": "Ridge",
            "version": "2.8",
            "revision": 1,
        }
        entry["needs"] = [
            {
                "kind": "oamod",
                "id": "ridge",
                "version": "2.8",
                "revision": 1,
                "sha256": state.beside_mod_sha,
            }
        ]
    elif detail:
        entry["needs"] = []
    return entry


def battle_entry(kind, player, game_id):
    """One feed row for a battle lookup. Its kind is the one the game asked for."""
    return {
        "id": LOOKUP_ID,
        "revision": 1,
        "title": "Lookup",
        "source": "capture",
        "visibility": "public",
        "recorded_at": "2026-11-04T14:32:10Z",
        "length_ms": 40,
        "game": {
            "kind": kind,
            "game_id": game_id,
            "planet": REAL_PLANET,
            "sector": SECTOR,
        },
        "map": {"name": "Lookup"},
        "players": [
            {
                "name": player,
                "side": "arm",
                "colour": 0,
                "team": 1,
                "result": "won",
                "kills": 1,
            }
        ],
        "mod": None,
        "uploader": "",
        "downloads": 1,
        "files": {
            "oademo": {"sha256": "a" * 64, "size": 1},
            "tad": {"sha256": "b" * 64, "size": 1},
        },
    }


def index_of(replay):
    """The place of an id, or none."""
    prefix = "r_"
    if not replay.startswith(prefix) or len(replay) != len(prefix) + 22:
        return None
    body = replay[len(prefix) :]
    if not body.isdigit():
        return None
    index = int(body)
    if index < 0 or index >= ITEM_COUNT:
        return None
    return index


def make_handler(state):
    """A handler for one feed. It writes nothing of its own."""

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            return

        def _send(self, status, body, content_type, cache=False):
            data = body if isinstance(body, bytes) else body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            if cache:
                self.send_header("Cache-Control", "public, max-age=60")
            self.end_headers()
            self.wfile.write(data)

        def _json(self, status, payload, cache=False):
            self._send(status, json.dumps(payload), "application/json", cache)

        def _json_headers(self, status, payload, headers):
            data = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            split = urlsplit(self.path)
            path = split.path
            if path == "/api/v1/challenges/R13CODE":
                with state.lock:
                    state.polled = True
                    state.challenge_solved = True
                self._json(
                    200,
                    {
                        "code": "R13CODE",
                        "state": "solved",
                        "expires": "2027-01-01T00:00:00Z",
                    },
                )
                return
            if ".tad" in path:
                with state.lock:
                    state.tad = True
                self._send(404, b"", "text/plain")
                return
            if path == "/content/v1/replays":
                query = parse_qs(split.query)
                with state.lock:
                    state.queries.append(split.query)
                    state.query_times.append(time.monotonic())
                    asked_kind = query.get("kind", [""])[0]
                    asked_player = query.get("player", [""])[0]
                    # A battle lookup names a kind and a player. The catalogue
                    # and My battles do not name a kind.
                    if asked_kind and asked_player and asked_kind != KIND:
                        state.battle_queries.append(split.query)
                        if len(state.battle_queries) == 1:
                            page = {
                                "replays": 1,
                                "total": 0,
                                "next": "",
                                "items": [],
                                "beacon": True,
                            }
                        else:
                            page = {
                                "replays": 1,
                                "total": 1,
                                "next": "",
                                "items": [battle_entry(asked_kind, asked_player, state.lookup_game)],
                                "beacon": True,
                            }
                        # The same address answers twice, first empty and then listed.
                        # A cached empty page would hide the second answer.
                        self._json(200, page, cache=False)
                        return
                    if state.mode == "backoff":
                        self._json(
                            503,
                            {
                                "error": "replays_unavailable",
                                "message": "Replays are not available.",
                            },
                        )
                        return
                    rows = []
                    for index in range(ITEM_COUNT):
                        rows.append(entry_of(state, index, False))
                    planet = query.get("planet", [""])[0]
                    if planet:
                        rows = [row for row in rows if row["game"]["planet"] == planet]
                    sort = query.get("sort", ["newest"])[0]
                    if sort == "longest":
                        rows.sort(key=lambda row: row["length_ms"], reverse=True)
                    elif sort == "downloads":
                        rows.sort(key=lambda row: row["downloads"], reverse=True)
                    first_text = query.get("first", [str(PAGE_SIZE)])[0]
                    try:
                        first = int(first_text)
                    except ValueError:
                        first = PAGE_SIZE
                    first = max(1, min(first, 100))
                    after = query.get("after", [""])[0]
                    start = PAGE_SIZE if after == "page2" else 0
                    page = rows[start : start + first]
                    cursor = "page2" if start == 0 and len(rows) > first else ""
                    body = {
                        "replays": 1,
                        "total": len(rows),
                        "next": cursor,
                        "items": page,
                        "beacon": True,
                    }
                self._json(200, body, cache=True)
                return
            if path.startswith("/content/v1/replays/"):
                replay = path[len("/content/v1/replays/") :]
                index = index_of(replay)
                if index is None:
                    self._json(404, {"error": "unknown_replay", "message": "No such replay."})
                    return
                with state.lock:
                    state.entry_gets.append((replay, state.bumped))
                    body = entry_of(state, index, True)
                self._json(200, body, cache=True)
                return
            if path.startswith("/v1/r/") and path.endswith(".oademo"):
                digest = path[len("/v1/r/") : -len(".oademo")]
                with state.lock:
                    recorded = state.recorded
                    real = state.real_sha
                    beside = state.beside_bytes
                    beside_sha = hashlib.sha256(beside).hexdigest() if beside else ""
                if beside_sha and digest == beside_sha:
                    self._send(200, beside, "application/octet-stream")
                    return
                if digest == real:
                    self._send(200, recorded, "application/octet-stream")
                    return
                if digest == BAD_SHA:
                    self._send(200, BAD_BYTES, "application/octet-stream")
                    return
                self._send(404, b"", "text/plain")
                return
            self._send(404, b"", "text/plain")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(length)
            path = urlsplit(self.path).path
            if path == "/graphql":
                try:
                    payload = json.loads(raw.decode("utf-8"))
                except json.JSONDecodeError:
                    self._json(400, {"error": "bad_request", "message": "The body is not JSON."})
                    return
                variables = payload.get("variables") or {}
                accepted = (
                    isinstance(payload.get("query"), str)
                    and "mutation Login" in payload["query"]
                    and variables.get("handle") == "nj"
                    and variables.get("password") == "made-up-secret"
                )
                with state.lock:
                    state.logins.append(accepted)
                if not accepted:
                    self._json(
                        200,
                        {
                            "data": {
                                "login": {
                                    "ok": False,
                                    "error": "Invalid handle or password.",
                                    "account": None,
                                }
                            }
                        },
                    )
                    return
                self._json_headers(
                    200,
                    {
                        "data": {
                            "login": {
                                "ok": True,
                                "error": None,
                                "account": {"handle": "nj"},
                            }
                        }
                    },
                    [("Set-Cookie", "cp_session=made-up-token; Path=/; HttpOnly; Secure")],
                )
                return
            if path == "/content/v1/replays":
                auth = self.headers.get("Authorization", "")
                with state.lock:
                    state.uploads.append(raw)
                    state.upload_auths.append(auth)
                    count = len(state.uploads)
                if count == 1:
                    self._json(
                        201,
                        {
                            "id": "r_0123456789abcdefghijAB",
                            "revision": 1,
                            "title": "Show Down",
                            "source": "upload",
                            "visibility": "public",
                            "recorded_at": "2026-11-04T14:32:10Z",
                            "length_ms": 1,
                            "game": {"kind": "outpost"},
                            "map": {"name": "Show Down"},
                            "players": [],
                            "downloads": 0,
                            "files": {
                                "oademo": {"sha256": "a" * 64, "size": 1},
                                "tad": {"sha256": "b" * 64, "size": 1},
                            },
                        },
                    )
                    return
                if count == 2:
                    self._json(
                        401,
                        {"error": "signed_out", "message": "Sign in to share a replay."},
                    )
                    return
                self._json(500, {"error": "unexpected", "message": "An extra upload arrived."})
                return
            if path.startswith("/api/v1/downloads/") and path.endswith("/result"):
                with state.lock:
                    state.results.append(raw.decode("utf-8", errors="replace"))
                self._send(204, b"", "text/plain")
                return
            if path != "/api/v1/downloads":
                self._send(404, b"", "text/plain")
                return
            try:
                payload = json.loads(raw.decode("utf-8"))
            except json.JSONDecodeError:
                self._json(400, {"error": "bad_request", "message": "The body is not JSON."})
                return
            with state.lock:
                state.keys.append(payload)
                download_id = f"dl{state.next_download}"
                state.next_download += 1
                package = payload.get("package", "")
                release = payload.get("release")
                kind = payload.get("kind", "")
                if state.mode == "wait" and package == replay_id(0):
                    self._json_headers(
                        429,
                        {"error": "rate_limited", "message": "Slow down."},
                        [("Retry-After", "30")],
                    )
                    return
                if state.mode == "rate" and package == replay_id(0) and state.rate_sent < 1:
                    state.rate_sent += 1
                    self._json_headers(
                        429,
                        {"error": "rate_limited", "message": "Slow down."},
                        [("Retry-After", "1")],
                    )
                    return
                if (
                    state.mode == "challenge"
                    and package == replay_id(0)
                    and not state.challenge_solved
                ):
                    self._json(
                        428,
                        {
                            "challenge": {
                                "code": "R13CODE",
                                "verify": f"{state.base}/check",
                                "expires": "2027-01-01T00:00:00Z",
                                "poll": 2,
                            }
                        },
                    )
                    return
                if (
                    kind == "oademo"
                    and package == replay_id(1)
                    and not state.bumped
                    and release == 1
                ):
                    state.bumped = True
                    self._json(
                        409,
                        {
                            "error": "release_mismatch",
                            "message": "The release changed.",
                        },
                    )
                    return
                if kind != "oademo":
                    self._json(404, {"error": "unknown_package", "message": "No such package."})
                    return
                if package == replay_id(0):
                    digest = (
                        hashlib.sha256(state.beside_bytes).hexdigest()
                        if state.mode == "beside" and state.beside_bytes
                        else state.real_sha
                    )
                elif package == replay_id(1) and state.bumped:
                    digest = state.real_sha
                elif package == replay_id(2):
                    digest = BAD_SHA
                else:
                    self._json(404, {"error": "unknown_package", "message": "No such package."})
                    return
                answer = {
                    "download": download_id,
                    "expires": "2027-01-01T00:00:00Z",
                    "url": f"{state.base}/v1/r/{digest}.oademo",
                }
            self._json(201, answer)

    return Handler


def serve(state):
    """Serves the feed on 127.0.0.1 and returns the server."""
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


def run_game(native, game_dir, work, case_dir, extra, keep_user=False):
    """Runs one start. Its output is printed. The status is returned."""
    user = case_dir / "user"
    data = case_dir / "data"
    if not keep_user:
        if user.exists():
            shutil.rmtree(user)
        if data.exists():
            shutil.rmtree(data)
    user.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
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


def decompress_zstd(data):
    """The bytes of one zstd frame. Uses the standard library, then a module, then the zstd tool."""
    try:
        from compression import zstd
        return zstd.decompress(data)
    except ImportError:
        pass
    try:
        import zstandard
        return zstandard.ZstdDecompressor().decompress(data)
    except ImportError:
        pass
    completed = subprocess.run(
        ["zstd", "-d", "-c"], input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    if completed.returncode == 0:
        return completed.stdout
    raise RuntimeError("no zstd decoder: compression.zstd, zstandard, or the zstd tool")


def tad_sector_types(data):
    """The sector types of a TA Demo recording, in order."""
    chunks = []
    position = 0
    while position + 2 <= len(data):
        length = int.from_bytes(data[position:position + 2], "little")
        if length < 2 or position + length > len(data):
            break
        chunks.append(data[position:position + length])
        position += length
    if len(chunks) < 2 or len(chunks[1]) < 6:
        raise RuntimeError("the recording has no sector count")
    count = int.from_bytes(chunks[1][2:6], "little")
    types = []
    for body in chunks[2:2 + count]:
        if len(body) < 6:
            raise RuntimeError("the recording has a short sector")
        types.append(int.from_bytes(body[2:6], "little"))
    return types


def multipart_replay(raw):
    """The replay part of an upload. The boundary is the first line, and it is not in the file."""
    if not raw.startswith(b"--"):
        raise RuntimeError("the upload is not multipart")
    line_end = raw.find(b"\r\n")
    if line_end < 4:
        raise RuntimeError("the upload has no boundary")
    boundary = raw[2:line_end]
    for piece in raw.split(b"--" + boundary):
        if b'name="replay"' not in piece:
            continue
        head, marker, body = piece.partition(b"\r\n\r\n")
        if not marker:
            raise RuntimeError("the replay part has no body")
        if body.endswith(b"\r\n"):
            body = body[:-2]
        return body
    raise RuntimeError("the upload has no replay part")


def uploaded_recording(raw):
    """The recording inside an uploaded .oademo."""
    blob = multipart_replay(raw)
    with zipfile.ZipFile(io.BytesIO(blob)) as archive:
        names = archive.namelist()
        for name in names:
            if not name.startswith("recording"):
                continue
            stored = archive.read(name)
            if name.endswith(".zst"):
                return decompress_zstd(stored)
            return stored
    raise RuntimeError(f"the upload holds {names}, not a recording")


def recording_game_id(path):
    """The game id in a replay's description. The kind's name is whatever the file says."""
    with zipfile.ZipFile(path) as archive:
        meta = json.loads(archive.read("meta.json"))
    game = meta.get("game")
    if not isinstance(game, dict):
        raise RuntimeError("the battle has no game")
    kind = game.get("kind")
    body = game.get(kind) if isinstance(kind, str) else None
    if not isinstance(body, dict):
        raise RuntimeError("the battle has no game id")
    game_id = body.get("game_id")
    if not isinstance(game_id, str) or not game_id:
        raise RuntimeError("the battle has no game id")
    return game_id


def battle_lookup(query):
    """True when a feed query is a battle lookup: a kind, a player and a page of 100."""
    parsed = parse_qs(query)
    kind = parsed.get("kind", [""])[0]
    player = parsed.get("player", [""])[0]
    first = parsed.get("first", [""])[0]
    return bool(kind) and kind != KIND and bool(player) and first == "100"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--tool", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--work-dir", required=True)
    args = parser.parse_args()
    work = Path(args.work_dir)
    work.mkdir(parents=True, exist_ok=True)
    pictures = work / "pictures"
    pictures.mkdir(parents=True, exist_ok=True)
    source = work / "core-prime-source.oademo"
    recorded = run_game(
        args.native,
        args.game_dir,
        work,
        work / "record",
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

    state = FeedState(source.read_bytes())
    listed_battle = Path(args.work_dir).parent / "listed-battle.oademo"
    if listed_battle.is_file():
        state.lookup_game = recording_game_id(listed_battle)
    server = serve(state)
    facts = work / "core-prime.facts"
    values = {
        "case": "play",
        "watch_id": replay_id(0),
        "mismatch_id": replay_id(1),
        "bad_id": replay_id(2),
        "planet": REAL_PLANET,
        "other_planet": OTHER_PLANET,
        "sector": SECTOR,
        "kind": KIND,
        "watch_sha": state.real_sha,
        "watch_file": WATCH_TITLE + ".oademo",
        "mismatch_file": MISMATCH_TITLE + ".oademo",
        "bad_file": BAD_TITLE + ".oademo",
    }
    if listed_battle.is_file():
        values["battle_file"] = str(listed_battle)
    try:
        write_facts(facts, values)
        played = run_game(
            args.native,
            args.game_dir,
            work,
            work / "play",
            [
                "--check-replays-core-prime",
                "--core-prime-api",
                state.base,
                "--snapshot",
                str(pictures / "replays.ppm"),
            ],
        )
        saved = work / "play" / "user" / "Recordings" / "Core Prime"
        watch_path = saved / values["watch_file"]
        mismatch_path = saved / values["mismatch_file"]
        bad_path = saved / values["bad_file"]
        watch_keys = [key for key in state.keys if key.get("package") == values["watch_id"]]
        mismatch_keys = [key for key in state.keys if key.get("package") == values["mismatch_id"]]
        bad_keys = [key for key in state.keys if key.get("package") == values["bad_id"]]
        queries = "\n".join(state.queries)
        reread = any(replay == values["mismatch_id"] and bumped for replay, bumped in state.entry_gets)
        play_ok = (
            played.returncode == 0
            and "core prime: feed result" in played.stdout
            and "core prime: page 2" in played.stdout
            and "core prime: pages held" in played.stdout
            and "core prime: planet Ashmere" in played.stdout
            and "core prime: planet Cinder" in played.stdout
            and "core prime: downloaded" in played.stdout
            and "core prime: on this computer" in played.stdout
            and "core prime: changed release read again" in played.stdout
            and "core prime: file differs" in played.stdout
            and "core prime: watching" in played.stdout
            and "sort=newest&after=page2&first=48" in queries
            and "sort=newest&planet=Ashmere&first=48" in queries
            and "sort=newest&planet=Cinder&first=48" in queries
            and len(state.keys) == 4
            and len(state.results) == 3
            and len(watch_keys) == 1
            and watch_keys[0].get("kind") == "oademo"
            and watch_keys[0].get("release") == 3
            and watch_keys[0].get("sha256") == state.real_sha
            and len(mismatch_keys) == 2
            and mismatch_keys[0].get("kind") == "oademo"
            and mismatch_keys[0].get("release") == 1
            and mismatch_keys[0].get("sha256") == FAKE_SHA
            and mismatch_keys[1].get("release") == 2
            and mismatch_keys[1].get("sha256") == state.real_sha
            and len(bad_keys) == 1
            and bad_keys[0].get("kind") == "oademo"
            and bad_keys[0].get("sha256") == BAD_SHA
            and reread
            and not state.tad
            and watch_path.is_file()
            and hashlib.sha256(watch_path.read_bytes()).hexdigest() == state.real_sha
            and mismatch_path.is_file()
            and hashlib.sha256(mismatch_path.read_bytes()).hexdigest() == state.real_sha
            and not bad_path.exists()
        )
        if not play_ok:
            print(
                f"play failed: status {played.returncode}, keys {len(state.keys)}, "
                f"results {len(state.results)}, queries {state.queries}, tad {state.tad}",
                file=sys.stderr,
            )
            return 1

        state.uploads.clear()
        state.upload_auths.clear()
        state.logins.clear()
        state.queries.clear()
        state.battle_queries.clear()
        share_values = {
            "case": "share",
            "handle": "nj",
            "password": "made-up-secret",
            "source": str(source),
            "share_id": "r_0123456789abcdefghijAB",
        }
        if listed_battle.is_file():
            share_values["battle_file"] = str(listed_battle)
        write_facts(facts, share_values)
        shared = run_game(
            args.native,
            args.game_dir,
            work,
            work / "share",
            [
                "--check-replays-core-prime",
                "--core-prime-api",
                state.base,
                "--snapshot",
                str(pictures / "replays.ppm"),
            ],
        )

        def holds_secret(path):
            """True when a file under path holds the made-up token or password."""
            if not path.exists():
                return False
            paths = [path] if path.is_file() else list(path.rglob("*"))
            for child in paths:
                if not child.is_file():
                    continue
                data = child.read_bytes()
                if b"made-up-token" in data or b"made-up-secret" in data:
                    return True
            return False

        share_dir = work / "share"
        leaked = (
            holds_secret(share_dir / "user")
            or holds_secret(share_dir / "data")
            or holds_secret(share_dir / "prefs.conf")
        )
        first = state.uploads[0] if state.uploads else b""
        auths = list(state.upload_auths)
        mine = any("player=nj" in query and "kind=" not in query for query in state.queries)
        planted_path = work / "share" / "user" / "Recordings" / "share.tad"
        planted = planted_path.read_bytes() if planted_path.is_file() else b""
        uploaded = b""
        upload_error = ""
        try:
            if first:
                uploaded = uploaded_recording(first)
        except (OSError, RuntimeError, zipfile.BadZipFile, ValueError) as error:
            upload_error = str(error)
        planted_types = tad_sector_types(planted) if planted else []
        uploaded_types = tad_sector_types(uploaded) if uploaded else []
        lookups = [query for query in state.battle_queries if battle_lookup(query)]
        pictures_ok = True
        for name in (
            "replays-sign-in.ppm",
            "replays-card-share.ppm",
            "replays-share.ppm",
            "replays-card-listed.ppm",
        ):
            picture = pictures / name
            if not picture.is_file() or picture.stat().st_size < 100000:
                pictures_ok = False
        share_ok = (
            shared.returncode == 0
            and "core prime: signed in" in shared.stdout
            and "core prime: signed out" in shared.stdout
            and "core prime: refused before sending" in shared.stdout
            and "core prime: my battles" in shared.stdout
            and "core prime: battle absent" in shared.stdout
            and "core prime: battle listed" in shared.stdout
            and len(state.uploads) == 2
            and len(auths) == 2
            and auths[0] == "Bearer made-up-token"
            and auths[1] == "Bearer made-up-token"
            and b'name="replay"' in first
            and b'filename="replay.oademo"' in first
            and b'name="title"' in first
            and b'name="visibility"' in first
            and b"public" in first
            and state.logins == [True, True]
            and mine
            and not leaked
            and not upload_error
            and 6 in planted_types
            and CHAT_MARK in planted
            and ADDRESS_TEXT in planted
            and 6 not in uploaded_types
            and CHAT_MARK in uploaded
            and ADDRESS_TEXT not in uploaded
            and len(lookups) >= 2
            and len(state.battle_queries) == len(lookups)
            and pictures_ok
        )
        if not share_ok:
            print(
                f"share failed: status {shared.returncode}, uploads {len(state.uploads)}, "
                f"logins {len(state.logins)}, mine {mine}, leaked {leaked}, "
                f"planted sectors {planted_types}, uploaded sectors {uploaded_types}, "
                f"lookups {len(lookups)}, upload {upload_error!r}, pictures {pictures_ok}",
                file=sys.stderr,
            )
            return 1

        state.keys.clear()
        state.results.clear()
        restart_values = dict(values)
        restart_values["case"] = "restart"
        write_facts(facts, restart_values)
        restarted = run_game(
            args.native,
            args.game_dir,
            work,
            work / "play",
            [
                "--check-replays-core-prime",
                "--core-prime-api",
                state.base,
            ],
            keep_user=True,
        )
        if (
            restarted.returncode != 0
            or "core prime: still downloaded" not in restarted.stdout
            or state.keys
        ):
            print(
                f"restart failed: status {restarted.returncode}, keys {len(state.keys)}",
                file=sys.stderr,
            )
            return 1

        def run_case(case_name, case_dir, case_values):
            state.mode = case_name
            state.keys.clear()
            state.results.clear()
            state.queries.clear()
            state.query_times.clear()
            state.polled = False
            state.rate_sent = 0
            state.challenge_solved = False
            write_facts(facts, case_values)
            return run_game(
                args.native,
                args.game_dir,
                work,
                case_dir,
                [
                    "--check-replays-core-prime",
                    "--core-prime-api",
                    state.base,
                ],
            )

        backed = run_case("backoff", work / "backoff", {"case": "backoff"})
        window = (
            [stamp for stamp in state.query_times if stamp - state.query_times[0] <= 10.0]
            if state.query_times
            else []
        )
        if (
            backed.returncode != 0
            or "core prime: backed off" not in backed.stdout
            or len(window) > 2
            or len(window) < 1
        ):
            print(
                f"backoff failed: status {backed.returncode}, requests {len(window)}",
                file=sys.stderr,
            )
            return 1

        rate_values = {
            "case": "rate",
            "watch_id": replay_id(0),
            "watch_file": WATCH_TITLE + ".oademo",
        }
        rated = run_case("rate", work / "rate", rate_values)
        rate_path = work / "rate" / "user" / "Recordings" / "Core Prime" / rate_values["watch_file"]
        if (
            rated.returncode != 0
            or "core prime: rate saved" not in rated.stdout
            or not rate_path.is_file()
            or hashlib.sha256(rate_path.read_bytes()).hexdigest() != state.real_sha
        ):
            print(f"rate failed: status {rated.returncode}", file=sys.stderr)
            return 1

        challenge_values = {
            "case": "challenge",
            "watch_id": replay_id(0),
            "watch_file": WATCH_TITLE + ".oademo",
        }
        challenged = run_case("challenge", work / "challenge", challenge_values)
        challenge_path = (
            work / "challenge" / "user" / "Recordings" / "Core Prime" / challenge_values["watch_file"]
        )
        if (
            challenged.returncode != 0
            or "core prime: challenge saved" not in challenged.stdout
            or not state.polled
            or not challenge_path.is_file()
            or hashlib.sha256(challenge_path.read_bytes()).hexdigest() != state.real_sha
        ):
            print(
                f"challenge failed: status {challenged.returncode}, polled {state.polled}",
                file=sys.stderr,
            )
            return 1

        wait_values = {
            "case": "wait",
            "watch_id": replay_id(0),
            "watch_file": WATCH_TITLE + ".oademo",
            "other_id": replay_id(1),
        }
        waited = run_case("wait", work / "wait", wait_values)
        if (
            waited.returncode != 0
            or "core prime: other card" not in waited.stdout
            or "core prime: cancelled" not in waited.stdout
            or len(state.keys) != 1
        ):
            print(
                f"wait failed: status {waited.returncode}, keys {len(state.keys)}",
                file=sys.stderr,
            )
            return 1

        content_facts = work / "replays-content.facts"
        # The content check builds its Ridge Reach pack from the map of a
        # recorded game in the working folder.
        shutil.copy2(source, work / "replays-content-source.oademo")
        write_facts(content_facts, {"case": "prepare"})
        prepared = run_game(
            args.native,
            args.game_dir,
            work,
            work / "prepare",
            ["--check-replays-content"],
        )
        if prepared.returncode != 0 or "replays content: packages written" not in prepared.stdout:
            return fail("the packages were not written")
        ridge_source = work / "core-prime-ridge.oademo"
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
        (
            registry_state,
            serve_dir,
            _old_sha,
            _new_sha,
            _mutator_sha,
            ridge_sha,
            _gone_sha,
            public,
        ) = ridge_registry.publish(args.tool, work)
        registry = ridge_registry.serve(serve_dir, registry_state)
        try:
            (serve_dir / "registry.yaml").write_text(
                ridge_registry.registry_text(registry_state.base, public),
                encoding="utf-8",
            )
            ridge_new_sha = ridge_registry.sha256_file(work / "ridge-29.oamod")
            state.beside_bytes = ridge_source.read_bytes()
            state.beside_mod_sha = ridge_sha
            state.mode = "beside"
            state.keys.clear()
            state.results.clear()
            write_facts(
                facts,
                {
                    "case": "beside",
                    "watch_id": replay_id(0),
                    "watch_file": WATCH_TITLE + ".oademo",
                    "descriptor": registry_state.base + "/registry.yaml",
                    "registry": ridge_registry.REGISTRY_ID,
                    "ridge_sha": ridge_sha,
                    "ridge_new_sha": ridge_new_sha,
                },
            )
            beside = run_game(
                args.native,
                args.game_dir,
                work,
                work / "beside",
                [
                    "--check-replays-core-prime",
                    "--core-prime-api",
                    state.base,
                ],
            )
            with registry_state.lock:
                posts = list(registry_state.posts)
            parsed = []
            for body in posts:
                try:
                    parsed.append(json.loads(body))
                except json.JSONDecodeError:
                    parsed.append({})
            asked_ridge = sum(1 for item in parsed if item.get("sha256") == ridge_sha)
            asked_newer = any(item.get("sha256") == ridge_new_sha for item in parsed)
            file_sha = hashlib.sha256(state.beside_bytes).hexdigest()
            tables = re.findall(
                r"^demo unit table: (\d+) recorded, \1 matched, 0 missing, 0 extra here$",
                beside.stdout,
                re.MULTILINE,
            )
            beside_ok = (
                beside.returncode == 0
                and "Needs Ridge 2.8" in beside.stdout
                and "You have 2.9" in beside.stdout
                and "Get Ridge 2.8 and watch" in beside.stdout
                and "core prime: ridge 2.8 playing" in beside.stdout
                and "core prime: 2.9 chosen" in beside.stdout
                and "core prime: end screen stayed until Main Menu" in beside.stdout
                and "core prime: the next start played nothing" in beside.stdout
                and asked_ridge == 1
                and not asked_newer
                and len(tables) >= 1
                and len(state.keys) == 1
                and state.keys[0].get("kind") == "oademo"
                and state.keys[0].get("package") == replay_id(0)
                and state.keys[0].get("sha256") == file_sha
            )
            if not beside_ok:
                print(
                    f"beside failed: status {beside.returncode}, "
                    f"ridge keys {asked_ridge}, newer {asked_newer}, "
                    f"feed keys {len(state.keys)}, tables {len(tables)}",
                    file=sys.stderr,
                )
                return 1
        finally:
            registry.shutdown()
            registry.server_close()
    finally:
        server.shutdown()
        server.server_close()

    offline_values = dict(values)
    offline_values["case"] = "offline"
    write_facts(facts, offline_values)
    offline = run_game(
        args.native,
        args.game_dir,
        work,
        work / "offline",
        [
            "--check-replays-core-prime",
            "--core-prime-api",
            state.base,
            "--snapshot",
            str(pictures / "replays.ppm"),
        ],
    )
    pictures_ok = all(
        (pictures / name).is_file()
        for name in (
            "replays-computer-empty.ppm",
            "replays-core-list.ppm",
            "replays-core-planet.ppm",
            "replays-core-offline.ppm",
            "replays-sign-in.ppm",
            "replays-share.ppm",
            "replays-card-share.ppm",
            "replays-card-listed.ppm",
        )
    )
    if (
        offline.returncode != 0
        or "Core Prime cannot be reached." not in offline.stdout
        or not pictures_ok
    ):
        return fail(f"offline failed: status {offline.returncode}, pictures {pictures_ok}")
    print("core prime: check passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:
        print(f"core prime: {error}", file=sys.stderr)
        sys.exit(1)
