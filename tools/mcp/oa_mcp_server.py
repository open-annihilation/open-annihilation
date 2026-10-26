#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""An MCP server that drives Open Annihilation through its automation endpoint.

An MCP client (Claude Code, Claude Desktop or any other) starts this script
and speaks the Model Context Protocol to it over standard input and output.
The script starts the game with --fark, or attaches to a game already started
so, connects to the automation endpoint on the loopback address, and offers
the endpoint's requests as MCP tools: what the screen and its controls show,
clicks on a control by its name, keys, typed text, the wheel, a picture of
the frame, the preferences, the match, the battle room and the events. The
game is driven only as the endpoint drives it, with the events of a keyboard,
a mouse and a touch screen; docs/automation.md says what the endpoint does.

The script starts no program but Open Annihilation's own executable, passes
the game only the command-line options it allows, gives it a preferences
file of its own unless told otherwise, and connects only to the loopback
address. Driven by input, the game does what a player could do at the
keyboard; tools/mcp/README.md says what that reaches.

The script needs Python 3.8 or later and nothing beyond its standard library.

    python3 tools/mcp/oa_mcp_server.py               serve MCP on stdin/stdout
    python3 tools/mcp/oa_mcp_server.py --self-test   check it against a stand-in endpoint

tools/mcp/README.md says how to register it with an MCP client.
"""
import argparse
import base64
import collections
import io
import json
import math
import os
from pathlib import Path
import queue
import shlex
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import zlib

SERVER_NAME = "open-annihilation"
SERVER_VERSION = "1.0.0"
# The MCP revisions the server speaks, the newest first; a client asking for
# another is answered with the newest.
MCP_VERSIONS = ("2025-06-18", "2025-03-26", "2024-11-05")
# The automation protocol's version.
AUTOMATION_VERSION = 1
# The largest JSON part and payload the endpoint sends, in bytes.
MAX_JSON_BYTES = 1 << 20
MAX_PAYLOAD_BYTES = 64 << 20
# The longest header line, its line feed included.
MAX_HEADER_BYTES = 96
# Seconds to wait for an answer, for the game to write its endpoint file,
# and for it to end after quit.
ANSWER_SECONDS = 30.0
START_SECONDS = 120.0
QUIT_SECONDS = 30.0
# The longest wait a tool call may ask for, in seconds: the server answers
# one call at a time, so a longer one would hold every other call up.
MAX_WAIT_SECONDS = 600.0
# The frames a press_key call may hold keys down for in all: 30 seconds
# at the game's 30 frames a second.
MAX_HELD_FRAMES = 900
# Seconds between two looks while waiting for a screen or a control.
POLL_SECONDS = 0.2
# The events kept for read_events; older ones are dropped first.
EVENTS_KEPT = 2000
# The lines of the game's log a failure or game_status quotes.
LOG_TAIL_LINES = 40
# The most clicks select_list_item makes on a list's scroll arrows.
MAX_SCROLL_CLICKS = 64
# The game's canvas on the menus.
CANVAS_WIDTH = 640
CANVAS_HEIGHT = 480
# JSON-RPC error codes.
PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL_ERROR = -32603

REPO_DIR = Path(__file__).resolve().parents[2]
# The names Open Annihilation's executable has: on Windows, and everywhere
# else (a macOS bundle's binary in Contents/MacOS included). A compatibility
# layer named by --runner may run the Windows one elsewhere.
EXECUTABLE_NAMES = ("open-annihilation.exe", "open-annihilation")
# The one a build on this system makes, which the build folders are searched for.
EXECUTABLE_NAME = EXECUTABLE_NAMES[0] if os.name == "nt" else EXECUTABLE_NAMES[1]
# A Total Annihilation installation holds this archive (any case).
GAME_ARCHIVE = "totala1.hpi"
# The first line of a preferences file the game reads; a file of that line
# alone holds no setting, so the game plays on its defaults.
PREFERENCES_HEADER = "open-annihilation-preferences 1\n"
# The endpoint file the game writes is under 200 bytes; a larger file is not one.
MAX_ENDPOINT_FILE_BYTES = 4096
# The hosts the endpoint may be on: the game serves only the loopback address.
LOOPBACK_HOSTS = {"127.0.0.1": "127.0.0.1", "::1": "::1", "localhost": "127.0.0.1"}
# The endpoint's token: 128 bits as 32 lowercase hexadecimal digits.
TOKEN_DIGITS = 32

# The command-line options game_launch's arguments may hold, by what each
# takes: FLAG nothing; a set, nothing or one of those words after an =; VALUE
# a word that names no file, as the next argument; PATH a file or folder, as
# the next argument, which must lie in the server's scratch folder. Every
# other option is refused: --fark and its options, --preferences-file and
# --game-dir are the server's own, and the options that run a check, a
# benchmark or a headless run cannot go with --fark.
FLAG, VALUE, PATH = "flag", "value", "path"
LAUNCH_OPTIONS = {
    "--skip-intro": FLAG, "--mute": FLAG, "--first-run": FLAG, "--touch-controls": FLAG,
    "--native-density": FLAG, "--experimental-3d": FLAG, "--no-hardware-acceleration": FLAG,
    "--hardware-acceleration": frozenset({"off", "basic", "full"}), "--max-fps": VALUE,
    "--resolution": VALUE, "--base-game": FLAG, "--no-mutators": FLAG, "--content": VALUE,
    "--trace-input": FLAG, "--debug-order-lines": FLAG, "--seed": VALUE,
    "--status-stdout": FLAG, "--status-file": PATH, "--quit-after-game": frozenset({"after-score"}),
    "--host": FLAG, "--join": VALUE, "--player-name": VALUE, "--game-name": VALUE,
    "--game-password": VALUE, "--map": VALUE, "--side": VALUE, "--team": VALUE, "--colour": VALUE,
    "--player-limit": VALUE, "--tournament": frozenset({"on", "off"}),
    "--closed": frozenset({"on", "off"}),
    "--user-folder": PATH, "--data-dir": PATH, "--mod-dir": PATH, "--open": PATH,
    "--install-mod": PATH,
}
# The options whose value the server's log leaves out.
SECRET_OPTIONS = {"--game-password"}

# SDL scancodes by key name (USB HID usage numbers, the same in SDL 2 and 3).
SCANCODES = {
    "return": 40, "enter": 40, "escape": 41, "esc": 41, "backspace": 42, "tab": 43,
    "space": 44, "minus": 45, "-": 45, "equals": 46, "=": 46, "leftbracket": 47, "[": 47,
    "rightbracket": 48, "]": 48, "backslash": 49, "\\": 49, "semicolon": 51, ";": 51,
    "apostrophe": 52, "'": 52, "grave": 53, "`": 53, "comma": 54, ",": 54,
    "period": 55, ".": 55, "slash": 56, "/": 56, "capslock": 57,
    "printscreen": 70, "scrolllock": 71, "pause": 72, "insert": 73, "home": 74,
    "pageup": 75, "delete": 76, "end": 77, "pagedown": 78,
    "right": 79, "left": 80, "down": 81, "up": 82,
    "kp_divide": 84, "kp_multiply": 85, "kp_minus": 86, "kp_plus": 87, "kp_enter": 88,
    "kp_period": 99,
    "ctrl": 224, "lctrl": 224, "control": 224, "shift": 225, "lshift": 225,
    "alt": 226, "lalt": 226, "gui": 227, "lgui": 227, "cmd": 227, "win": 227,
    "rctrl": 228, "rshift": 229, "ralt": 230, "rgui": 231,
}
SCANCODES.update({chr(ord("a") + n): 4 + n for n in range(26)})
SCANCODES.update({str(n): 30 + (n - 1) for n in range(1, 10)})
SCANCODES["0"] = 39
SCANCODES.update({f"f{n}": 58 + (n - 1) for n in range(1, 13)})
SCANCODES.update({f"kp_{n}": 89 + (n - 1) for n in range(1, 10)})
SCANCODES["kp_0"] = 98
MODIFIER_KEYS = {"ctrl", "lctrl", "control", "shift", "lshift", "alt", "lalt", "gui", "lgui",
                 "cmd", "win", "rctrl", "rshift", "ralt", "rgui"}


def log(message):
    """Writes a line to standard error, which MCP clients keep as the server's log."""
    print(f"oa-mcp: {message}", file=sys.stderr, flush=True)


class ToolError(Exception):
    """A tool call that cannot be done; its message is the tool's answer."""


class EndpointError(ToolError):
    """A request the endpoint refused, with the endpoint's error code."""

    def __init__(self, op, error):
        self.code = error.get("code", "")
        super().__init__(f"{op}: refused {self.code}: {error.get('message', '')}".rstrip(": "))


# --- The automation protocol's frames ---------------------------------------


def encode_frame(message, payload=b""):
    """Returns the frame of a JSON object and a payload, with route '-'."""
    body = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    crc = zlib.crc32(body + payload) & 0xFFFFFFFF
    header = b"AUTO/1 - %d %d %08x\n" % (len(body), len(payload), crc)
    return header + body + payload


class FrameStream:
    """Reads whole frames from a socket."""

    def __init__(self, sock):
        self._sock = sock
        self._buffer = bytearray()

    def _fill(self):
        chunk = self._sock.recv(1 << 16)
        if not chunk:
            raise ConnectionError("the endpoint closed the connection")
        self._buffer += chunk

    def read(self):
        """Returns the next frame's JSON object and payload."""
        while True:
            newline = self._buffer.find(b"\n")
            if newline >= 0:
                break
            if len(self._buffer) > MAX_HEADER_BYTES:
                raise ConnectionError("a frame header is too long")
            self._fill()
        header = bytes(self._buffer[:newline]).decode("ascii", "replace").split(" ")
        if len(header) != 5 or header[0] != "AUTO/1":
            raise ConnectionError(f"not a frame header: {' '.join(header)!r}")
        json_length, payload_length = int(header[2]), int(header[3])
        if json_length > MAX_JSON_BYTES or payload_length > MAX_PAYLOAD_BYTES:
            raise ConnectionError("a frame is larger than the protocol allows")
        size = newline + 1 + json_length + payload_length
        while len(self._buffer) < size:
            self._fill()
        body = bytes(self._buffer[newline + 1:newline + 1 + json_length])
        payload = bytes(self._buffer[newline + 1 + json_length:size])
        del self._buffer[:size]
        if (zlib.crc32(body + payload) & 0xFFFFFFFF) != int(header[4], 16):
            raise ConnectionError("a frame's CRC does not match")
        message = json.loads(body.decode("utf-8")) if body else {}
        if not isinstance(message, dict):
            raise ConnectionError("a frame's JSON is not an object")
        return message, payload


def loopback_address(address):
    """Returns the host and port to connect to for an endpoint address; refuses any but the loopback's.

    The address is 127.0.0.1:PORT, [::1]:PORT or localhost:PORT, as the game
    serves only the loopback address; localhost is reached as 127.0.0.1, so
    that no name is looked up.
    """
    if not isinstance(address, str):
        raise ToolError("the endpoint's address is not text")
    host, _, port = address.rpartition(":")
    if host.startswith("[") and host.endswith("]"):
        host = host[1:-1]
    if (host not in LOOPBACK_HOSTS or not port.isascii() or not port.isdigit() or len(port) > 5
            or not 0 < int(port) < 65536):
        raise ToolError("the endpoint's address is not on the loopback address (127.0.0.1, ::1 or "
                        "localhost and a port), the only one the game serves")
    return LOOPBACK_HOSTS[host], int(port)


class Endpoint:
    """One connection to the automation endpoint: requests, their answers and the events."""

    def __init__(self, address, token, client="oa-mcp"):
        self._sock = socket.create_connection(loopback_address(address), timeout=10)
        self._stream = FrameStream(self._sock)
        self._lock = threading.Lock()
        self._next_id = 1
        self._waiting = {}
        self.events = collections.deque(maxlen=EVENTS_KEPT)
        self.closed_reason = None
        # The hello is answered before the reader starts, so a refusal is read
        # here, within the connection's timeout.
        try:
            self._sock.sendall(encode_frame({"id": 0, "op": "hello", "versions": [AUTOMATION_VERSION],
                                             "client": client, "token": token}))
            answer, _ = self._stream.read()
        except (OSError, ValueError):
            self._sock.close()
            raise
        if not answer.get("ok"):
            self._sock.close()
            error = answer.get("error")
            raise EndpointError("hello", error if isinstance(error, dict) else {})
        self._sock.settimeout(None)
        self.hello = answer
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        try:
            while True:
                message, payload = self._stream.read()
                if "event" in message:
                    self.events.append(message)
                    continue
                with self._lock:
                    waiter = self._waiting.pop(message.get("id"), None)
                if waiter is not None:
                    waiter.put((message, payload))
        except (OSError, ConnectionError, ValueError) as error:
            self.closed_reason = str(error) or type(error).__name__
        with self._lock:
            waiters, self._waiting = list(self._waiting.values()), {}
        for waiter in waiters:
            waiter.put(None)

    @property
    def connected(self):
        return self.closed_reason is None

    def request(self, op, fields=None):
        """Sends a request and returns its answer and payload; a refusal raises EndpointError.

        The fields go into the request beside its id and op, which they
        cannot replace, and the answer is waited for ANSWER_SECONDS at most.
        """
        if not self.connected:
            raise ToolError(f"the endpoint connection is closed ({self.closed_reason})")
        waiter = queue.Queue(maxsize=1)
        with self._lock:
            request_id = self._next_id
            self._next_id += 1
            self._waiting[request_id] = waiter
        try:
            self._sock.sendall(encode_frame({**(fields or {}), "id": request_id, "op": op}))
            answer = waiter.get(timeout=ANSWER_SECONDS)
        except queue.Empty:
            raise ToolError(f"{op}: no answer within {ANSWER_SECONDS:g} s") from None
        finally:
            with self._lock:
                self._waiting.pop(request_id, None)
        if answer is None:
            raise ToolError(f"{op}: the endpoint connection closed ({self.closed_reason})")
        message, payload = answer
        if not message.get("ok"):
            error = message.get("error")
            raise EndpointError(op, error if isinstance(error, dict) else {})
        return message, payload

    def close(self):
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()


# --- Pictures ----------------------------------------------------------------


def png_from_rgb(width, height, pixels, max_width=None):
    """Returns a compressed PNG of 8-bit RGB pixels, shrunk by whole steps to at most max_width."""
    step = 1
    if max_width and width > max_width:
        step = -(-width // max_width)
    out_width = -(-width // step)
    stride = width * 3
    rows = []
    for y in range(0, height, step):
        row = pixels[y * stride:(y + 1) * stride]
        if step == 1:
            rows.append(b"\x00" + row)
        else:
            picked = bytearray(out_width * 3)
            for n, x in enumerate(range(0, width, step)):
                picked[n * 3:n * 3 + 3] = row[x * 3:x * 3 + 3]
            rows.append(b"\x00" + bytes(picked))
    out_height = len(rows)

    def chunk(kind, data):
        return (len(data).to_bytes(4, "big") + kind + data
                + (zlib.crc32(kind + data) & 0xFFFFFFFF).to_bytes(4, "big"))

    header = (out_width.to_bytes(4, "big") + out_height.to_bytes(4, "big")
              + bytes((8, 2, 0, 0, 0)))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b"")), out_width, out_height


# --- The game ------------------------------------------------------------------


def inside(path, folder):
    """Whether path, its links followed, lies in folder (or is it)."""
    try:
        Path(path).resolve().relative_to(Path(folder).resolve())
    except ValueError:
        return False
    return True


def game_executable(path):
    """Returns path, resolved, when it is Open Annihilation's executable; refuses any other file.

    The executable is named open-annihilation or open-annihilation.exe, and
    inside a macOS bundle it is the binary in Contents/MacOS; a bundle's
    folder (*.app) names that binary.
    """
    path = Path(path)
    if path.suffix == ".app" and path.is_dir():
        path = path / "Contents" / "MacOS" / EXECUTABLE_NAMES[1]
    resolved = path.resolve()
    if not resolved.is_file():
        raise ToolError(f"{path} is not a file")
    if resolved.name not in EXECUTABLE_NAMES:
        raise ToolError(f"{path} is not Open Annihilation's executable, which is named "
                        f"{' or '.join(EXECUTABLE_NAMES)}")
    bundle = next((parent for parent in resolved.parents if parent.suffix == ".app"), None)
    if bundle is not None and resolved.parent != bundle / "Contents" / "MacOS":
        raise ToolError(f"{path} is not the binary in {bundle.name}/Contents/MacOS")
    return resolved


def find_executables(repo_dir=REPO_DIR):
    """Returns this system's open-annihilation executables in the repository's build folders, newest first."""
    found = []
    for build in sorted(Path(repo_dir).glob("build*")):
        if not build.is_dir():
            continue
        candidates = [build / EXECUTABLE_NAME, *sorted(build.glob(f"*/{EXECUTABLE_NAME}"))]
        if sys.platform == "darwin":
            candidates += sorted(build.glob(f"*.app/Contents/MacOS/{EXECUTABLE_NAME}"))
        for candidate in candidates:
            if candidate.is_file() and candidate.resolve() not in found:
                found.append(candidate.resolve())
    return sorted(found, key=lambda path: path.stat().st_mtime, reverse=True)


def game_folder(folder, named_by):
    """Returns folder, resolved, when it holds a Total Annihilation installation; refuses any other."""
    path = Path(folder).resolve()
    if not path.is_dir():
        raise ToolError(f"{named_by} {folder} is not a folder")
    try:
        holds_game = any(entry.name.lower() == GAME_ARCHIVE and entry.is_file() for entry in path.iterdir())
    except OSError as error:
        raise ToolError(f"cannot read {named_by} {folder}: {error.strerror}") from None
    if not holds_game:
        raise ToolError(f"{named_by} {folder} holds no {GAME_ARCHIVE}: name the Total Annihilation "
                        f"installation")
    return path


def launch_arguments(arguments, folder, scratch):
    """Returns game_launch's arguments when LAUNCH_OPTIONS allows each; refuses any other.

    A path is read as the game reads it, from folder, its working folder, and
    must lie in scratch, the server's scratch folder.
    """
    if any("\0" in argument for argument in arguments):
        raise ToolError("an argument cannot hold a zero character")
    checked = []
    index = 0
    while index < len(arguments):
        argument = arguments[index]
        name, equals, suffix = argument.partition("=")
        kind = LAUNCH_OPTIONS.get(name) if name.startswith("--") else None
        if kind is None:
            if name.startswith("--fark") or name in ("--preferences-file", "--game-dir"):
                raise ToolError(f"arguments cannot hold {name}: the server sets it (game_dir and "
                                f"preferences_file choose the folder and the file)")
            raise ToolError(f"arguments cannot hold {argument!r}; it takes only the options "
                            f"{', '.join(sorted(LAUNCH_OPTIONS))}")
        if isinstance(kind, frozenset):
            if equals and suffix not in kind:
                raise ToolError(f"{name} takes ={' or ='.join(sorted(kind))}, or nothing")
            checked.append(argument)
        elif equals:
            raise ToolError(f"{name} takes {'no value' if kind == FLAG else 'its value as the next argument'}")
        elif kind == FLAG:
            checked.append(argument)
        else:
            value = arguments[index + 1] if index + 1 < len(arguments) else ""
            if not value or value.startswith("-"):
                raise ToolError(f"{name} needs a value as the next argument")
            if kind == PATH and not inside(Path(folder) / value, scratch):
                raise ToolError(f"{name} {value} lies outside the server's scratch folder {scratch}, "
                                f"where every file arguments names must be")
            checked += [argument, value]
            index += 1
        index += 1
    return checked


def read_endpoint_file(path):
    """Returns the address, token and pid of an endpoint file the game wrote; refuses any other file.

    The game writes a small JSON object of the protocol's version, a loopback
    address, a token of 32 hexadecimal digits and its process id, owned by
    the player and writable by no one else. Nothing else in the file is
    read, and a refusal quotes none of it.
    """
    path = Path(path)
    refused = f"{path} is not an endpoint file the game wrote"
    try:
        info = path.stat()
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_ENDPOINT_FILE_BYTES:
            raise ToolError(f"{refused}: the game's is a file of under {MAX_ENDPOINT_FILE_BYTES} bytes")
        if hasattr(os, "getuid") and (info.st_uid != os.getuid() or info.st_mode & 0o022):
            raise ToolError(f"{refused}: the game's is the player's own and writable by no one else")
        with open(path, "rb") as file:
            description = json.loads(file.read(MAX_ENDPOINT_FILE_BYTES).decode("utf-8"))
    except OSError as error:
        raise ToolError(f"cannot read the endpoint file {path}: {error.strerror}") from None
    except ValueError:
        raise ToolError(f"{refused}: it is not JSON") from None
    if not isinstance(description, dict) or description.get("version") != AUTOMATION_VERSION:
        raise ToolError(f"{refused}: it holds no version {AUTOMATION_VERSION}")
    token, pid = description.get("token"), description.get("pid")
    if (not isinstance(token, str) or len(token) != TOKEN_DIGITS
            or any(digit not in "0123456789abcdef" for digit in token)):
        raise ToolError(f"{refused}: it holds no token of {TOKEN_DIGITS} hexadecimal digits")
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        raise ToolError(f"{refused}: it holds no process id")
    try:
        loopback_address(description.get("address"))
    except ToolError as error:
        raise ToolError(f"{refused}: {error}") from None
    return description["address"], token, pid


def shown_command(command):
    """The command as the server's log shows it, without the values of SECRET_OPTIONS."""
    return " ".join("***" if index and command[index - 1] in SECRET_OPTIONS else part
                    for index, part in enumerate(command))


class Game:
    """The game this server started, or the endpoint file of one it attached to, and the connection."""

    def __init__(self, runner=(), executable=None, preferences_dirs=(), repo_dir=REPO_DIR):
        # A command that starts the executable, such as the compatibility
        # layer a cross build runs through; empty runs it directly.
        self.runner = list(runner)
        # The executable the server's command line names, which game_launch
        # starts unless asked for another of the known ones.
        self.chosen_executable = game_executable(executable) if executable else None
        # The folders the server's command line names, whose preferences
        # files game_launch may give the game besides the scratch folder's.
        self.preferences_dirs = [Path(folder).resolve() for folder in preferences_dirs]
        self.repo_dir = Path(repo_dir)
        self._scratch = None
        self.launches = 0
        self.process = None
        self.executable = None
        self.log_path = None
        self.preferences_file = None
        self.folder = None
        self.endpoint_file = None
        self.endpoint = None
        self.pid = None

    @property
    def scratch(self):
        """The server's own folder in the system's temporary folder, made at first use."""
        if self._scratch is None:
            self._scratch = Path(tempfile.mkdtemp(prefix="oa-mcp-")).resolve()
        return self._scratch

    def running(self):
        return self.process is not None and self.process.poll() is None

    def log_tail(self, lines=LOG_TAIL_LINES):
        if not self.log_path or not Path(self.log_path).is_file():
            return ""
        text = Path(self.log_path).read_text(encoding="utf-8", errors="replace")
        return "\n".join(text.splitlines()[-lines:])

    def known_executables(self):
        """The executables game_launch may start: the server's --executable, OA_EXECUTABLE, then the builds'.

        The first is the one started when no other is asked for.
        """
        known = [self.chosen_executable] if self.chosen_executable else []
        if os.environ.get("OA_EXECUTABLE"):
            try:
                known.append(game_executable(os.environ["OA_EXECUTABLE"]))
            except ToolError as error:
                raise ToolError(f"OA_EXECUTABLE: {error}") from None
        known += find_executables(self.repo_dir)
        return list(dict.fromkeys(known))

    def choose_executable(self, executable):
        """Returns the known executable to start: the first, or the one executable names."""
        known = self.known_executables()
        if not known:
            raise ToolError(f"no {EXECUTABLE_NAME} executable: build it (./run.sh), or name it with the "
                            f"server's --executable option or OA_EXECUTABLE")
        if not executable:
            return known[0]
        if not Path(executable).exists():
            raise ToolError(f"executable {executable} does not exist; the known ones are "
                            f"{', '.join(map(str, known))}")
        wanted = game_executable(executable)
        if wanted not in known:
            raise ToolError(f"executable {executable} is not one the server found: game_launch starts only "
                            f"{', '.join(map(str, known))}")
        return wanted

    def preferences(self, preferences_file):
        """Returns preferences_file, resolved, when it is a file the game may be given; refuses any other."""
        path = Path(preferences_file).resolve()
        if not path.is_file():
            raise ToolError(f"preferences_file {preferences_file} is not a file: name one that exists, or "
                            f"leave it out for a new one in the server's scratch folder")
        allowed = [self.scratch, *self.preferences_dirs]
        if not any(inside(path, folder) for folder in allowed):
            raise ToolError(f"preferences_file {preferences_file} lies outside the server's scratch folder "
                            f"and the folders its --preferences-dir names: "
                            f"{', '.join(map(str, allowed))}")
        return path

    def launch(self, executable, game_dir, headless, preferences_file, skip_intro, mute, arguments,
               timeout):
        if self.running():
            raise ToolError("a game is already running; call game_quit first")
        # Everything is checked before anything changes, so a refused launch
        # leaves the game connected, if one is, as it was.
        executable = self.choose_executable(executable)
        if game_dir:
            game_dir = game_folder(game_dir, "game_dir")
        elif os.environ.get("OA_GAME_DIR"):
            game_dir = game_folder(os.environ["OA_GAME_DIR"], "OA_GAME_DIR")
        if preferences_file:
            preferences_file = self.preferences(preferences_file)
        folder = self.scratch / f"game-{self.launches + 1}"
        arguments = launch_arguments(arguments, folder, self.scratch)
        self.disconnect()
        self.forget_launch()
        self.launches += 1
        folder.mkdir()
        self.folder = folder
        self.endpoint_file = folder / "automation.json"
        self.log_path = folder / "game.log"
        if not preferences_file:
            preferences_file = folder / "preferences.conf"
            preferences_file.write_text(PREFERENCES_HEADER, encoding="utf-8")
        self.preferences_file = preferences_file
        command = [*self.runner, str(executable), "--fark", "--fark-file", str(self.endpoint_file),
                   "--preferences-file", str(preferences_file)]
        if game_dir:
            command += ["--game-dir", str(game_dir)]
        if skip_intro:
            command.append("--skip-intro")
        if mute:
            command.append("--mute")
        command += arguments
        environment = dict(os.environ)
        if headless:
            environment.update(SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                               SDL_RENDER_DRIVER="software")
        log_file = open(self.log_path, "wb")
        self.process = subprocess.Popen(command, cwd=folder, env=environment,
                                        stdin=subprocess.DEVNULL, stdout=log_file,
                                        stderr=subprocess.STDOUT)
        log_file.close()
        self.executable = executable
        self.pid = self.process.pid
        log(f"started {shown_command(command)} (pid {self.pid})")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.endpoint_file.is_file():
                self.connect(self.endpoint_file)
                return
            if self.process.poll() is not None:
                raise ToolError(f"the game exited with {self.process.returncode} before serving the "
                                f"endpoint; its log:\n{self.log_tail()}")
            time.sleep(POLL_SECONDS)
        self.stop()
        raise ToolError(f"the game wrote no endpoint file within {timeout:g} s; its log:\n{self.log_tail()}")

    def attach(self, endpoint_file):
        """Connects to a game started elsewhere, forgetting the one launched here, which has ended."""
        if self.running():
            raise ToolError("a game launched here is running: call game_quit before attaching to another")
        address, token, pid = read_endpoint_file(endpoint_file)
        self.disconnect()
        self.forget_launch()
        self.pid = pid
        self._connect(endpoint_file, address, token)

    def connect(self, endpoint_file):
        """Connects to the game launched here through the endpoint file it wrote."""
        address, token, _ = read_endpoint_file(endpoint_file)
        self.disconnect()
        self._connect(endpoint_file, address, token)

    def _connect(self, endpoint_file, address, token):
        self.endpoint_file = Path(endpoint_file)
        try:
            self.endpoint = Endpoint(address, token)
        except (OSError, ValueError) as error:
            raise ToolError(f"cannot connect to the endpoint at {address}: {error}") from None

    def disconnect(self):
        if self.endpoint is not None:
            self.endpoint.close()
            self.endpoint = None

    def require(self):
        if self.endpoint is None:
            raise ToolError("no game is connected: call game_launch, or game_attach for a game "
                            "started with --fark")
        if not self.endpoint.connected:
            detail = ""
            if self.process is not None and self.process.poll() is not None:
                detail = f"; the game exited with {self.process.returncode}"
            raise ToolError(f"the endpoint connection closed ({self.endpoint.closed_reason}){detail}")
        return self.endpoint

    def stop(self):
        self.disconnect()
        if self.running():
            self.process.kill()
            self.process.wait()

    def forget_launch(self):
        """Forgets the game launched last, which has ended, and removes its folder in the scratch folder."""
        if self.running():
            return
        if self.folder is not None:
            shutil.rmtree(self.folder, ignore_errors=True)
        self.process = self.executable = self.log_path = self.folder = self.endpoint_file = None
        self.preferences_file = self.pid = None

    def remove_scratch(self):
        """Removes the scratch folder and all in it, once no game launched here runs."""
        if self._scratch is not None and not self.running():
            shutil.rmtree(self._scratch, ignore_errors=True)
            self._scratch = None
            self.folder = None


# --- The tools -----------------------------------------------------------------


def text_result(value):
    if not isinstance(value, str):
        value = json.dumps(value, ensure_ascii=False, indent=1)
    return {"content": [{"type": "text", "text": value}]}


def strip_answer(answer):
    return {key: value for key, value in answer.items() if key not in ("id", "ok")}


def compact_control(control):
    """The fields of a control a caller needs to find and press it."""
    out = {"name": control.get("name"), "kind": control.get("kind")}
    if control.get("dialog"):
        out["dialog"] = control["dialog"]
    if control.get("window"):
        out["window"] = control["window"]
    out["rect"] = control.get("rect")
    if not control.get("enabled", True):
        out["enabled"] = False
    if not control.get("visible", True):
        out["visible"] = False
    if control.get("text"):
        out["text"] = control["text"]
    if control.get("focused"):
        out["focused"] = True
    if "checked" in control:
        out["checked"] = control["checked"]
    if control.get("kind") == "list":
        for key in ("items", "item_keys", "selected", "first_visible", "rows", "scroll_up", "scroll_down"):
            if key in control and control[key] is not None:
                out[key] = control[key]
    return out


def rect_centre(rect):
    x, y, width, height = rect
    return x + width // 2, y + height // 2


def click_events(x, y, button="left", clicks=1, space="game"):
    events = [{"kind": "pointer_move", "x": x, "y": y, "space": space}]
    for click in range(1, clicks + 1):
        events.append({"kind": "button_down", "x": x, "y": y, "space": space, "button": button,
                       "clicks": click})
        events.append({"kind": "button_up", "x": x, "y": y, "space": space, "button": button,
                       "clicks": click})
    return events


def key_events(combo):
    """The key_down and key_up events of a key or a chord such as ctrl+shift+s."""
    names = combo.replace(" ", "").lower().split("+")
    if any(not name for name in names):
        raise ToolError(f"cannot read the key {combo!r}: + joins keys; write shift+= or kp_plus for a plus")
    scancodes = []
    for name in names:
        if name not in SCANCODES:
            raise ToolError(f"unknown key {name!r}; keys are letters, digits, f1-f12, the named keys "
                            f"(escape, return, space, tab, backspace, up, down, left, right, pageup, "
                            f"pagedown, home, end, insert, delete, ctrl, shift, alt, gui) and "
                            f"punctuation, joined by + for a chord")
        scancodes.append((name, SCANCODES[name]))
    events = [{"kind": "key_down", "scancode": code, "key": name} for name, code in scancodes]
    events += [{"kind": "key_up", "scancode": code, "key": name} for name, code in reversed(scancodes)]
    return events


class Tools:
    """The MCP tools, each a method named as the tool, and their schemas."""

    def __init__(self, game):
        self.game = game

    # -- helpers --

    def _screen(self):
        answer, _ = self.game.require().request("screen")
        return answer

    def _controls(self):
        answer, _ = self.game.require().request("controls")
        return answer.get("screen"), answer.get("controls", [])

    def _input(self, events, expect_screen=None):
        fields = {"events": events}
        if expect_screen:
            fields["expect_screen"] = expect_screen
        answer, _ = self.game.require().request("input", fields)
        return answer.get("consumed", {})

    def _after(self, consumed):
        screen = self._screen()
        return {"consumed": consumed, "screen": screen.get("screen"), "dialogs": screen.get("dialogs"),
                "focused": screen.get("focused")}

    @staticmethod
    def _find(controls, name=None, text=None, dialog=None):
        def matches(control, exact):
            if dialog is not None and control.get("dialog") != dialog:
                return False
            if name is not None:
                wanted, actual = name, control.get("name") or ""
            else:
                wanted, actual = text, control.get("text") or ""
            return actual == wanted if exact else actual.lower() == wanted.lower()

        for exact in (True, False):
            for control in controls:
                if matches(control, exact):
                    return control
        return None

    def _require_control(self, name=None, text=None, dialog=None):
        if not name and not text:
            raise ToolError("name a control with name or text")
        screen, controls = self._controls()
        control = self._find(controls, name, text, dialog)
        if control is None:
            listed = ", ".join(sorted({c.get("name") or "" for c in controls if c.get("name")}))
            raise ToolError(f"no control {name or text!r} on screen {screen}; listed: {listed}")
        return screen, control

    # -- the game --

    def game_launch(self, executable=None, game_dir=None, headless=False, preferences_file=None,
                    skip_intro=True, mute=False, arguments=(), timeout=START_SECONDS):
        self.game.launch(executable, game_dir, headless, preferences_file, skip_intro, mute,
                         list(arguments), timeout)
        hello = strip_answer(self.game.endpoint.hello)
        return text_result({"pid": self.game.pid, "executable": str(self.game.executable),
                            "built": time.strftime("%Y-%m-%d %H:%M:%S",
                                                   time.localtime(self.game.executable.stat().st_mtime)),
                            "log": str(self.game.log_path), "endpoint_file": str(self.game.endpoint_file),
                            "preferences_file": str(self.game.preferences_file), "hello": hello})

    def game_attach(self, file=None):
        file = file or os.environ.get("OA_FARK_FILE")
        if not file:
            raise ToolError("name the endpoint file the game was started with (--fark-file), or set "
                            "OA_FARK_FILE")
        self.game.attach(file)
        return text_result({"pid": self.game.pid, "endpoint_file": str(file),
                            "hello": strip_answer(self.game.endpoint.hello)})

    def game_status(self):
        endpoint = self.game.endpoint
        status = {"connected": bool(endpoint and endpoint.connected),
                  "launched_here": self.game.process is not None,
                  "pid": self.game.pid,
                  "running": self.game.running() if self.game.process else None,
                  "exit_code": (self.game.process.returncode
                                if self.game.process and self.game.process.poll() is not None else None),
                  "executable": str(self.game.executable) if self.game.executable else None,
                  "endpoint_file": str(self.game.endpoint_file) if self.game.endpoint_file else None,
                  "log": str(self.game.log_path) if self.game.log_path else None,
                  "preferences_file": str(self.game.preferences_file) if self.game.preferences_file else None,
                  "scratch": str(self.game.scratch)}
        try:
            status["executables"] = [str(path) for path in self.game.known_executables()]
        except ToolError as error:
            status["executables"] = str(error)
        if endpoint and not endpoint.connected:
            status["closed_reason"] = endpoint.closed_reason
        if self.game.log_path:
            status["log_tail"] = self.game.log_tail()
        return text_result(status)

    def game_quit(self, force=False):
        process = self.game.process
        if force:
            self.game.stop()
            return text_result("the game was stopped")
        try:
            self.game.require().request("quit")
        except ToolError as error:
            if process is None or process.poll() is not None:
                raise
            log(f"quit was not answered ({error}); stopping the game")
            self.game.stop()
            return text_result("the game did not answer quit and was stopped")
        self.game.disconnect()
        if process is None:
            return text_result("quit sent")
        try:
            status = process.wait(timeout=QUIT_SECONDS)
        except subprocess.TimeoutExpired:
            self.game.stop()
            return text_result(f"the game did not end within {QUIT_SECONDS:g} s of quit and was stopped")
        return text_result(f"the game ended with status {status}")

    # -- reading --

    def screen(self):
        return text_result(strip_answer(self._screen()))

    def controls(self, dialog=None, name_contains=None, include_disabled=True, include_hidden=False,
                 full=False):
        screen, controls = self._controls()
        picked = []
        for control in controls:
            if dialog is not None and control.get("dialog") != dialog:
                continue
            if name_contains and name_contains.lower() not in (
                    (control.get("name") or "") + " " + (control.get("text") or "")).lower():
                continue
            if not include_disabled and not control.get("enabled", True):
                continue
            if not include_hidden and not control.get("visible", True):
                continue
            picked.append(control if full else compact_control(control))
        return text_result({"screen": screen, "count": len(picked), "controls": picked})

    def prefs(self, names=None):
        fields = {"names": list(names)} if names else {}
        answer, _ = self.game.require().request("prefs", fields)
        return text_result(strip_answer(answer))

    def match(self, state_hash=False):
        answer, _ = self.game.require().request("match", {"state_hash": True} if state_hash else None)
        return text_result(strip_answer(answer))

    def room(self):
        answer, _ = self.game.require().request("room")
        return text_result(strip_answer(answer))

    def screenshot(self, source="presented", region=None, space="game", max_width=1280):
        fields = {"source": source, "format": "rgb", "space": space}
        if region:
            fields["region"] = list(region)
        endpoint = self.game.require()
        try:
            answer, payload = endpoint.request("frame", fields)
        except EndpointError as error:
            if error.code != "no_window" or source != "presented":
                raise
            fields["source"] = "composed"
            answer, payload = endpoint.request("frame", fields)
        image = answer["image"]
        png, width, height = png_from_rgb(image["width"], image["height"], payload, max_width)
        about = {"source": image.get("source"), "width": image["width"], "height": image["height"],
                 "frame": answer.get("frame"), "tick": answer.get("tick")}
        if (width, height) != (image["width"], image["height"]):
            about["shown_as"] = [width, height]
        return {"content": [{"type": "image", "data": base64.b64encode(png).decode("ascii"),
                             "mimeType": "image/png"},
                            {"type": "text", "text": json.dumps(about)}]}

    def frame_hash(self, source="presented", region=None, space="game"):
        fields = {"source": source, "hash": True, "space": space}
        if region:
            fields["region"] = list(region)
        answer, _ = self.game.require().request("frame", fields)
        return text_result(strip_answer(answer))

    # -- driving --

    def click_control(self, name=None, text=None, dialog=None, button="left", double=False,
                      allow_disabled=False):
        screen, control = self._require_control(name, text, dialog)
        if not control.get("enabled", True) and not allow_disabled:
            raise ToolError(f"control {control.get('name')!r} is disabled")
        if not control.get("rect"):
            raise ToolError(f"control {control.get('name')!r} has no place on the canvas")
        x, y = rect_centre(control["rect"])
        consumed = self._input(click_events(x, y, button, 2 if double else 1), screen)
        result = self._after(consumed)
        result["clicked"] = {"name": control.get("name"), "at": [x, y]}
        return text_result(result)

    def select_list_item(self, list_name, item, dialog=None, double=False):
        screen, control = self._require_control(list_name, None, dialog)
        if control.get("kind") != "list":
            raise ToolError(f"control {list_name!r} is a {control.get('kind')}, not a list")
        items = control.get("items") or []
        keys = control.get("item_keys") or []
        index = items.index(item) if item in items else (keys.index(item) if item in keys else -1)
        if index < 0:
            lowered = [entry.lower() for entry in items]
            index = lowered.index(item.lower()) if item.lower() in lowered else -1
        if index < 0:
            raise ToolError(f"list {list_name!r} has no item {item!r}; it holds {items}")
        for _ in range(MAX_SCROLL_CLICKS):
            first, rows = control.get("first_visible", 0), control.get("rows") or len(items)
            if first <= index < first + rows:
                break
            arrow = control.get("scroll_down") if index >= first + rows else control.get("scroll_up")
            if not arrow:
                raise ToolError(f"item {item!r} is out of view and list {list_name!r} has no scroll arrow")
            _, arrow_control = self._require_control(arrow, None, None)
            self._input(click_events(*rect_centre(arrow_control["rect"])), screen)
            _, control = self._require_control(list_name, None, dialog)
        else:
            raise ToolError(f"item {item!r} did not come into view")
        x0, y0, width, _ = control["rect"]
        row_height = control.get("row_height") or 1
        x = x0 + width // 2
        y = y0 + (index - control.get("first_visible", 0)) * row_height + row_height // 2
        consumed = self._input(click_events(x, y, "left", 2 if double else 1), screen)
        result = self._after(consumed)
        _, control = self._require_control(list_name, None, dialog)
        result["selected"] = control.get("selected")
        result["wanted_index"] = index
        return text_result(result)

    def click(self, x, y, space="game", button="left", double=False):
        return text_result(self._after(self._input(click_events(int(x), int(y), button,
                                                                2 if double else 1, space))))

    def drag(self, x1, y1, x2, y2, space="game", button="left", steps=8):
        steps = max(1, int(steps))
        events = [{"kind": "pointer_move", "x": x1, "y": y1, "space": space},
                  {"kind": "button_down", "x": x1, "y": y1, "space": space, "button": button, "clicks": 1}]
        for step in range(1, steps + 1):
            events.append({"kind": "pointer_move", "space": space,
                           "x": x1 + (x2 - x1) * step // steps, "y": y1 + (y2 - y1) * step // steps})
        events.append({"kind": "button_up", "x": x2, "y": y2, "space": space, "button": button, "clicks": 1})
        return text_result(self._after(self._input(events)))

    def move_pointer(self, x, y, space="game"):
        return text_result(self._after(self._input([{"kind": "pointer_move", "x": x, "y": y,
                                                     "space": space}])))

    def press_key(self, key, repeat=1, hold_frames=0):
        if repeat * hold_frames > MAX_HELD_FRAMES:
            raise ToolError(f"press_key holds keys for at most {MAX_HELD_FRAMES} frames in all "
                            f"(repeat times hold_frames)")
        events = key_events(key)
        consumed = None
        for _ in range(max(1, int(repeat))):
            if hold_frames:
                downs = [event for event in events if event["kind"] == "key_down"]
                ups = [event for event in events if event["kind"] == "key_up"]
                self._input(downs)
                self._wait_frames(int(hold_frames))
                consumed = self._input(ups)
            else:
                consumed = self._input(events)
        return text_result(self._after(consumed))

    def type_text(self, text, submit=False):
        events = [{"kind": "text", "text": text}]
        if submit:
            events += key_events("return")
        return text_result(self._after(self._input(events)))

    def scroll(self, dy, dx=0, x=None, y=None, space="game"):
        event = {"kind": "wheel", "dx": int(dx), "dy": int(dy)}
        if x is not None and y is not None:
            event.update(x=x, y=y, space=space)
        return text_result(self._after(self._input([event])))

    def send_input(self, events, expect_screen=None):
        return text_result(self._after(self._input(list(events), expect_screen)))

    # -- waiting and events --

    def wait_for(self, screen=None, dialog=None, control=None, gone=False, timeout=30):
        if not (screen or dialog or control):
            raise ToolError("name a screen, a dialog or a control to wait for")
        deadline = time.monotonic() + float(timeout)
        while True:
            shown = self._screen()
            names = set()
            if control:
                _, controls = self._controls()
                names = {c.get("name") for c in controls if c.get("visible", True)}
            met = ((not screen or shown.get("screen") == screen)
                   and (not dialog or dialog in (shown.get("dialogs") or []))
                   and (not control or control in names))
            if met != bool(gone):
                return text_result({"met": True, "screen": shown.get("screen"),
                                    "dialogs": shown.get("dialogs"), "frame": shown.get("frame"),
                                    "tick": shown.get("tick")})
            if time.monotonic() >= deadline:
                return {"isError": True, "content": [{"type": "text", "text": json.dumps(
                    {"met": False, "waited_seconds": timeout, "screen": shown.get("screen"),
                     "dialogs": shown.get("dialogs")})}]}
            time.sleep(POLL_SECONDS)

    def _wait_frames(self, frames):
        """Waits until the game has served that many more frames; returns the first and the last."""
        endpoint = self.game.require()
        first = endpoint.request("screen")[0].get("frame", 0)
        deadline = time.monotonic() + ANSWER_SECONDS
        frame = first
        while frame - first < frames and time.monotonic() < deadline:
            time.sleep(0.01)
            frame = endpoint.request("screen")[0].get("frame", frame)
        return first, frame

    def wait_frames(self, frames=30):
        first, frame = self._wait_frames(int(frames))
        return text_result({"from_frame": first, "to_frame": frame})

    def subscribe(self, events="all"):
        answer, _ = self.game.require().request("subscribe", {"events": events})
        return text_result(strip_answer(answer))

    def read_events(self, max_events=100, wait_seconds=0, kinds=None):
        endpoint = self.game.require()
        deadline = time.monotonic() + float(wait_seconds)
        while not endpoint.events and time.monotonic() < deadline:
            time.sleep(0.05)
        taken = []
        kept = []
        while endpoint.events and len(taken) < int(max_events):
            event = endpoint.events.popleft()
            if kinds and event.get("event") not in kinds:
                kept.append(event)
                continue
            taken.append(event)
        endpoint.events.extendleft(reversed(kept))
        return text_result({"events": taken, "still_waiting": len(endpoint.events)})

    def request(self, op, fields=None):
        answer, payload = self.game.require().request(op, fields)
        result = strip_answer(answer)
        if payload:
            result["payload_bytes"] = len(payload)
        return text_result(result)


SPACE = {"type": "string", "enum": ["game", "window"],
         "description": "game: the 640x480 canvas the menus are drawn on (default); window: the window's pixels"}
BUTTON = {"type": "string", "enum": ["left", "right", "middle"], "default": "left"}
REGION = {"type": "array", "items": {"type": "integer"}, "minItems": 4, "maxItems": 4,
          "description": "[x, y, width, height]"}


def schema(properties=None, required=()):
    return {"type": "object", "properties": properties or {}, "required": list(required),
            "additionalProperties": False}


# What a value of each schema type is, for checked().
SCHEMA_TYPES = {
    "string": ("text", lambda value: isinstance(value, str)),
    "boolean": ("true or false", lambda value: isinstance(value, bool)),
    "integer": ("a whole number", lambda value: (isinstance(value, int) and not isinstance(value, bool))
                or (isinstance(value, float) and value.is_integer())),
    "number": ("a number", lambda value: isinstance(value, (int, float)) and not isinstance(value, bool)
               and math.isfinite(value)),
    "array": ("a list", lambda value: isinstance(value, list)),
    "object": ("an object", lambda value: isinstance(value, dict)),
}


def checked(rule, value, where):
    """Returns value when it meets the schema rule, a whole float as an int; raises ToolError otherwise.

    It reads the parts of JSON Schema the tools' schemas use: type, enum,
    minimum, maximum, minLength, items, minItems, maxItems, properties,
    required, additionalProperties false and anyOf.
    """
    if "anyOf" in rule:
        for choice in rule["anyOf"]:
            try:
                return checked(choice, value, where)
            except ToolError:
                pass
        raise ToolError(f"{where} is none of the forms it takes")
    kind = rule.get("type")
    if kind is not None:
        what, test = SCHEMA_TYPES[kind]
        if not test(value):
            raise ToolError(f"{where} must be {what}")
        if kind == "integer":
            value = int(value)
    if "enum" in rule and value not in rule["enum"]:
        raise ToolError(f"{where} must be one of {', '.join(map(str, rule['enum']))}")
    if "minimum" in rule and value < rule["minimum"]:
        raise ToolError(f"{where} must be at least {rule['minimum']:g}")
    if "maximum" in rule and value > rule["maximum"]:
        raise ToolError(f"{where} must be at most {rule['maximum']:g}")
    if "minLength" in rule and len(value) < rule["minLength"]:
        raise ToolError(f"{where} must not be empty")
    if kind == "array":
        if len(value) < rule.get("minItems", 0):
            raise ToolError(f"{where} must hold at least {rule['minItems']} items")
        if len(value) > rule.get("maxItems", len(value)):
            raise ToolError(f"{where} must hold at most {rule['maxItems']} items")
        value = [checked(rule.get("items", {}), item, f"{where}[{index}]") for index, item in enumerate(value)]
    if kind == "object" and "properties" in rule:
        properties = rule["properties"]
        unknown = [key for key in value if key not in properties]
        if unknown and rule.get("additionalProperties", True) is False:
            raise ToolError(f"{where} takes no {', '.join(map(str, unknown))}")
        missing = [key for key in rule.get("required", ()) if key not in value]
        if missing:
            raise ToolError(f"{where} needs {', '.join(missing)}")
        value = {key: checked(properties[key], item, key) if key in properties else item
                 for key, item in value.items()}
    return value


def seconds(default, minimum=0):
    """A wait in seconds, at most MAX_WAIT_SECONDS."""
    return {"type": "number", "default": default, "minimum": minimum, "maximum": MAX_WAIT_SECONDS}


TOOLS = [
    ("game_launch", "Start Open Annihilation with the automation endpoint on (--fark) and connect to it. "
     "Starts only the game's own executable: the server's --executable, else OA_EXECUTABLE, else the "
     "newest build of this system's open-annihilation in the repository's build folders; executable may "
     "name another of those game_status lists. The game gets a new preferences file in the server's "
     "scratch folder unless preferences_file names one, and the installation OA_GAME_DIR names unless "
     "game_dir is given. headless runs on SDL's dummy drivers, with no window and no sound; frames can "
     "still be taken. Driving the game can change its settings, install packages and send chat other "
     "players see.",
     schema({"executable": {"type": "string",
                            "description": "one of the executables game_status lists"},
             "game_dir": {"type": "string",
                          "description": "the Total Annihilation installation, a folder holding totala1.hpi"},
             "headless": {"type": "boolean", "default": False},
             "preferences_file": {"type": "string",
                                  "description": "an existing preferences file in the server's scratch folder "
                                                 "or a folder its --preferences-dir names; left out, a new "
                                                 "one, so the player's stay untouched"},
             "skip_intro": {"type": "boolean", "default": True},
             "mute": {"type": "boolean", "default": False},
             "arguments": {"type": "array", "items": {"type": "string"}, "maxItems": 64,
                           "description": "more command-line options, such as --host, --join ADDRESS or "
                                          "--resolution 1280x720; tools/mcp/README.md lists those taken, "
                                          "and a file they name must lie in the scratch folder"},
             "timeout": seconds(START_SECONDS, 1)})),
    ("game_attach", "Connect to a game already started with --fark --fark-file FILE, on this machine's "
     "loopback address. Refused while a game launched here runs: call game_quit first.",
     schema({"file": {"type": "string", "description": "the endpoint file the game wrote (automation.json)"}})),
    ("game_status", "Whether a game is connected and running, its executable, the end of its log, the "
     "server's scratch folder and the executables game_launch may start.",
     schema()),
    ("game_quit", "Quit the game as the main menu's Exit does (leaving a match first). force kills it.",
     schema({"force": {"type": "boolean", "default": False}})),
    ("screen", "The screen shown (main_menu, single_player, skirmish, mp_battleroom, match, ...), the "
     "dialogs over it (top first), the focused control, the pointer and, in a match, the camera.",
     schema()),
    ("controls", "The controls that take the pointer, topmost first: name, kind, dialog, rect "
     "[x, y, w, h] on the 640x480 canvas, enabled, text, checked, and a list's items. Filter to keep the "
     "answer short.",
     schema({"dialog": {"type": "string", "description": "only this dialog's controls"},
             "name_contains": {"type": "string", "description": "only controls whose name or text holds this"},
             "include_disabled": {"type": "boolean", "default": True},
             "include_hidden": {"type": "boolean", "default": False},
             "full": {"type": "boolean", "default": False, "description": "every field the endpoint gives"}})),
    ("screenshot", "A picture of the frame the game presented (or composed) as PNG, whole or a region.",
     schema({"source": {"type": "string", "enum": ["presented", "composed"], "default": "presented"},
             "region": REGION, "space": SPACE,
             "max_width": {"type": "integer", "default": 1280, "minimum": 1, "maximum": 16384,
                           "description": "shrink the picture by whole steps to at most this width"}})),
    ("frame_hash", "The 64-bit FNV-1a hash and mean colour of the frame or a region, to compare frames "
     "without taking a picture.",
     schema({"source": {"type": "string", "enum": ["presented", "composed"], "default": "presented"},
             "region": REGION, "space": SPACE})),
    ("click_control", "Click a control at its centre by its name (as controls lists it), or by its text. "
     "The click is refused if another screen shows by the time it arrives. Answers the screen after it.",
     schema({"name": {"type": "string"}, "text": {"type": "string"},
             "dialog": {"type": "string", "description": "only a control of this dialog"},
             "button": BUTTON, "double": {"type": "boolean", "default": False},
             "allow_disabled": {"type": "boolean", "default": False}})),
    ("select_list_item", "Choose a list's row by its shown text or its key (item_keys), scrolling the "
     "list with its arrows until the row is in view.",
     schema({"list_name": {"type": "string"}, "item": {"type": "string"},
             "dialog": {"type": "string"}, "double": {"type": "boolean", "default": False}},
            ["list_name", "item"])),
    ("click", "Click at a point.",
     schema({"x": {"type": "integer"}, "y": {"type": "integer"}, "space": SPACE, "button": BUTTON,
             "double": {"type": "boolean", "default": False}}, ["x", "y"])),
    ("drag", "Press at one point, move to another and let go, as a selection box is drawn.",
     schema({"x1": {"type": "integer"}, "y1": {"type": "integer"}, "x2": {"type": "integer"},
             "y2": {"type": "integer"}, "space": SPACE, "button": BUTTON,
             "steps": {"type": "integer", "default": 8, "minimum": 1, "maximum": 256}},
            ["x1", "y1", "x2", "y2"])),
    ("move_pointer", "Move the game's pointer (never the desktop's).",
     schema({"x": {"type": "integer"}, "y": {"type": "integer"}, "space": SPACE}, ["x", "y"])),
    ("press_key", "Press and let go of a key or a chord: escape, return, space, f1, a, 5, up, "
     "ctrl+s, shift+left. hold_frames keeps it down for that many frames (camera scrolling).",
     schema({"key": {"type": "string"},
             "repeat": {"type": "integer", "default": 1, "minimum": 1, "maximum": 100},
             "hold_frames": {"type": "integer", "default": 0, "minimum": 0,
                             "maximum": MAX_HELD_FRAMES}}, ["key"])),
    ("type_text", "Type text into the field that takes it (click the field first); submit presses Return.",
     schema({"text": {"type": "string"}, "submit": {"type": "boolean", "default": False}}, ["text"])),
    ("scroll", "Turn the mouse wheel; dy positive turns it away from the player.",
     schema({"dy": {"type": "integer"}, "dx": {"type": "integer", "default": 0},
             "x": {"type": "integer"}, "y": {"type": "integer"}, "space": SPACE}, ["dy"])),
    ("send_input", "Send raw input events as the endpoint takes them (key_down, key_up, text, "
     "pointer_move, button_down, button_up, wheel, finger_*); see docs/automation.md.",
     schema({"events": {"type": "array", "items": {"type": "object"}},
             "expect_screen": {"type": "string"}}, ["events"])),
    ("wait_for", "Wait until a screen shows, a dialog is open or a control is listed (or, with gone, "
     "until it no longer is).",
     schema({"screen": {"type": "string"}, "dialog": {"type": "string"}, "control": {"type": "string"},
             "gone": {"type": "boolean", "default": False},
             "timeout": seconds(30)})),
    ("wait_frames", "Let the game run for a number of frames.",
     schema({"frames": {"type": "integer", "default": 30, "minimum": 0, "maximum": MAX_HELD_FRAMES,
                        "description": "at most 30 seconds of them are waited for"}})),
    ("prefs", "The preferences as the game holds them, all or the keys named.",
     schema({"names": {"type": "array", "items": {"type": "string"}}})),
    ("match", "The running match: pause, speed, each player's side, team, units, kills and losses, the "
     "outcome and winner; state_hash adds the digest a saved game carries.",
     schema({"state_hash": {"type": "boolean", "default": False}})),
    ("room", "The battle room: the game's name, host, seated players, map, options and chat.", schema()),
    ("subscribe", "Choose the events kept for read_events: \"all\", or a list of kinds (screen, player, "
     "ready, chat, loaded, match, pause, speed, alliance, game_over); an empty list stops them.",
     schema({"events": {"anyOf": [{"type": "string", "enum": ["all"]},
                                  {"type": "array", "items": {"type": "string"}}]}}, ["events"])),
    ("read_events", "Take the events received since the last call, oldest first.",
     schema({"max_events": {"type": "integer", "default": 100, "minimum": 1, "maximum": EVENTS_KEPT},
             "wait_seconds": dict(seconds(0), description="wait this long for one when none has come"),
             "kinds": {"type": "array", "items": {"type": "string"}}})),
    ("request", "Send any automation request by its op and fields, and answer its JSON.",
     schema({"op": {"type": "string", "minLength": 1},
             "fields": {"type": "object", "description": "the request's fields beside its id and op, which "
                                                         "the server sets"}}, ["op"])),
]

INSTRUCTIONS = """Drives Open Annihilation, a portable engine for Total Annihilation 3.1c, through its
automation endpoint. Start with game_launch (or game_attach). Then read the screen with screen and
controls, act with click_control (by the control's name), select_list_item, press_key, type_text,
click and drag, and check the result with screenshot, screen, match or room. Points are on the
640x480 canvas unless space is window. In a match the canvas is the whole window. Input goes through
the game's event queue as a device's would; nothing gives orders to the simulation directly. Call
game_quit when done."""


# --- MCP over standard input and output ---------------------------------------------


class Server:
    """Answers MCP's JSON-RPC requests, one per line."""

    def __init__(self, game=None):
        self.game = game or Game()
        self.tools = Tools(self.game)

    def handle(self, message):
        """Returns the answer to one JSON-RPC message, or None for a notification."""
        if not isinstance(message, dict) or message.get("jsonrpc") != "2.0" or "method" not in message:
            return self._error(message.get("id") if isinstance(message, dict) else None,
                               INVALID_REQUEST, "not a JSON-RPC 2.0 request")
        method = message["method"]
        if "id" not in message:
            return None
        message_id = message["id"]
        params = message.get("params") or {}
        if not isinstance(params, dict):
            return self._error(message_id, INVALID_PARAMS, "params must be an object")
        if method == "initialize":
            asked = params.get("protocolVersion")
            return self._result(message_id, {
                "protocolVersion": asked if asked in MCP_VERSIONS else MCP_VERSIONS[0],
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
                "instructions": INSTRUCTIONS})
        if method == "ping":
            return self._result(message_id, {})
        if method == "tools/list":
            return self._result(message_id, {"tools": [
                {"name": name, "description": description, "inputSchema": input_schema}
                for name, description, input_schema in TOOLS]})
        if method == "tools/call":
            arguments = params.get("arguments") or {}
            if not isinstance(arguments, dict):
                return self._error(message_id, INVALID_PARAMS, "arguments must be an object")
            return self._result(message_id, self.call(params.get("name"), arguments))
        return self._error(message_id, METHOD_NOT_FOUND, f"unknown method {method}")

    def call(self, name, arguments):
        """Calls a tool once its arguments meet its schema; any failure is the tool's error, never the server's end."""
        known = {tool[0]: tool[2] for tool in TOOLS}
        if not isinstance(name, str) or name not in known:
            return {"isError": True, "content": [{"type": "text", "text": f"unknown tool {name}"}]}
        try:
            return getattr(self.tools, name)(**checked(known[name], arguments, name))
        except ToolError as error:
            return {"isError": True, "content": [{"type": "text", "text": str(error)}]}
        except Exception as error:  # a tool's own fault must leave the server and the game running
            log(f"{name} failed:\n{traceback.format_exc().rstrip()}")
            return {"isError": True, "content": [{"type": "text",
                                                  "text": f"{name} failed: {type(error).__name__}: {error}"}]}

    def answer(self, message):
        """Returns handle's answer, or an internal error when handling the message failed."""
        try:
            return self.handle(message)
        except Exception as error:  # one bad message must not end the server
            log(f"a request failed:\n{traceback.format_exc().rstrip()}")
            return self._error(message.get("id") if isinstance(message, dict) else None,
                               INTERNAL_ERROR, f"{type(error).__name__}: {error}")

    @staticmethod
    def _result(message_id, result):
        return {"jsonrpc": "2.0", "id": message_id, "result": result}

    @staticmethod
    def _error(message_id, code, text):
        return {"jsonrpc": "2.0", "id": message_id, "error": {"code": code, "message": text}}

    def serve(self, reader, writer):
        for line in reader:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                answer = self._error(None, PARSE_ERROR, "not JSON")
            else:
                answers = [self.answer(item) for item in message] if isinstance(message, list) \
                    else self.answer(message)
                if isinstance(answers, list):
                    answers = [item for item in answers if item is not None] or None
                answer = answers
            if answer is not None:
                try:
                    text = json.dumps(answer, ensure_ascii=False)
                except (TypeError, ValueError) as error:
                    text = json.dumps(self._error(None, INTERNAL_ERROR, f"cannot write the answer: {error}"))
                writer.write(text + "\n")
                writer.flush()

    def shutdown(self):
        if self.game.process is not None:
            self.game.stop()
        else:
            self.game.disconnect()
        self.game.remove_scratch()


# --- The self-test -------------------------------------------------------------------


class StandInEndpoint:
    """A stand-in for the game's endpoint: a main menu whose SINGLE button opens Single Player."""

    TOKEN = "00112233445566778899aabbccddeeff"

    def __init__(self, folder):
        self.listener = socket.create_server(("127.0.0.1", 0))
        self.port = self.listener.getsockname()[1]
        self.address = "127.0.0.1:%d" % self.port
        self.file = self.write_file(Path(folder) / "automation.json")
        self.screen = "main_menu"
        self.frame = 0
        self.inputs = []
        self.requests = []
        self.list_first = 0
        threading.Thread(target=self._serve, daemon=True).start()

    def description(self, **changes):
        """The endpoint file's JSON as the game writes it, with any field changed."""
        return json.dumps(dict({"version": 1, "address": self.address, "token": self.TOKEN,
                                "pid": os.getpid()}, **changes))

    def write_file(self, path, **changes):
        """Writes an endpoint file as the game does, readable by its owner alone."""
        path.write_text(self.description(**changes), encoding="utf-8")
        os.chmod(path, 0o600)
        return path

    def _controls(self):
        if self.screen == "main_menu":
            return [{"name": "SINGLE", "kind": "button", "dialog": None, "rect": [100, 100, 80, 20],
                     "enabled": True, "visible": True, "text": "Single Player"},
                    {"name": "MAPS", "kind": "list", "dialog": None, "rect": [300, 100, 200, 40],
                     "enabled": True, "visible": True, "items": ["Alpha", "Beta", "Gamma", "Delta"],
                     "item_keys": ["a.tnt", "b.tnt", "c.tnt", "d.tnt"], "first_visible": self.list_first,
                     "rows": 2, "row_height": 20, "selected": -1, "scroll_up": "SLIDER.up",
                     "scroll_down": "SLIDER.down"},
                    {"name": "SLIDER.down", "kind": "button", "dialog": None, "rect": [500, 140, 10, 10],
                     "enabled": True, "visible": True},
                    {"name": "SLIDER.up", "kind": "button", "dialog": None, "rect": [500, 90, 10, 10],
                     "enabled": True, "visible": True}]
        return [{"name": "BACK", "kind": "button", "dialog": None, "rect": [10, 10, 40, 20],
                 "enabled": True, "visible": True}]

    def _answer(self, request):
        op = request.get("op")
        base = {"id": request.get("id"), "ok": True, "frame": self.frame, "tick": 0}
        if op == "screen":
            return dict(base, screen=self.screen, dialogs=[], focused=None), b""
        if op == "controls":
            return dict(base, screen=self.screen, controls=self._controls()), b""
        if op == "input":
            if request.get("expect_screen") not in (None, self.screen):
                return {"id": request["id"], "ok": False,
                        "error": {"code": "screen_changed", "message": "another screen shows"}}, b""
            self.inputs.append(request["events"])
            for event in request["events"]:
                if event["kind"] == "button_down" and self.screen == "main_menu":
                    if 100 <= event["x"] < 180 and 100 <= event["y"] < 120:
                        self.screen = "single_player"
                    elif event["x"] >= 500 and 140 <= event["y"] < 150:
                        self.list_first = min(2, self.list_first + 2)
            return dict(base, consumed={"frame": self.frame, "tick": 0}), b""
        if op == "frame":
            width, height = 4, 2
            return dict(base, image={"format": "rgb", "width": width, "height": height,
                                     "source": request.get("source")}), bytes(range(width * height * 3))
        if op == "subscribe":
            return dict(base, events=request.get("events")), b""
        if op == "quit":
            return base, b""
        return {"id": request.get("id"), "ok": False,
                "error": {"code": "unknown_op", "message": f"no operation {op}"}}, b""

    def _serve(self):
        while True:
            connection, _ = self.listener.accept()
            stream = FrameStream(connection)
            try:
                hello, _ = stream.read()
                if hello.get("token") != self.TOKEN:
                    connection.sendall(encode_frame({"id": hello.get("id"), "ok": False,
                                                     "error": {"code": "denied", "message": "token"}}))
                    connection.close()
                    continue
                connection.sendall(encode_frame({"id": hello.get("id"), "ok": True, "frame": 0, "tick": 0,
                                                 "version": 1, "tick_rate": 30}))
                while True:
                    request, _ = stream.read()
                    self.requests.append(request)
                    self.frame += 1
                    answer, payload = self._answer(request)
                    if request.get("op") == "subscribe":
                        connection.sendall(encode_frame({"event": "screen", "seq": 1, "frame": self.frame,
                                                         "tick": 0, "name": self.screen, "previous": None}))
                    connection.sendall(encode_frame(answer, payload))
            except (OSError, ConnectionError):
                connection.close()


# A stand-in for the game that game_launch starts through --runner: it keeps
# its command line, writes the endpoint file the stand-in endpoint serves
# where --fark-file says, readable by its owner alone, and waits to be ended.
STAND_IN_GAME = """import json, os, sys, time
arguments = sys.argv[1:]
with open({command!r}, "w", encoding="utf-8") as out:
    json.dump(arguments, out)
endpoint = arguments[arguments.index("--fark-file") + 1]
with open(endpoint + ".part", "w", encoding="utf-8") as out:
    out.write({description!r})
os.chmod(endpoint + ".part", 0o600)
os.replace(endpoint + ".part", endpoint)
time.sleep(60)
"""


def self_test():
    """Drives the server's tools against the stand-in endpoint; returns the process status."""
    failures = []

    def check(condition, what):
        if not condition:
            failures.append(what)

    folder = Path(tempfile.mkdtemp(prefix="oa-mcp-selftest-")).resolve()
    # The environment names no executable, installation or endpoint file of the machine's.
    kept_environment = {name: os.environ.pop(name) for name in ("OA_EXECUTABLE", "OA_GAME_DIR", "OA_FARK_FILE")
                        if name in os.environ}
    server = None
    try:
        stand_in = StandInEndpoint(folder)
        # A repository whose build folder holds this system's executable and,
        # newer, the other system's.
        build = folder / "repo" / "build"
        build.mkdir(parents=True)
        own = build / EXECUTABLE_NAME
        other = build / next(name for name in EXECUTABLE_NAMES if name != EXECUTABLE_NAME)
        own.write_bytes(b"")
        other.write_bytes(b"")
        os.utime(own, (1_000_000_000, 1_000_000_000))
        command_file = folder / "command.json"
        stand_in_game = folder / "stand_in_game.py"
        stand_in_game.write_text(STAND_IN_GAME.format(command=str(command_file),
                                                      description=stand_in.description()), encoding="utf-8")
        preferences_dir = folder / "preferences"
        preferences_dir.mkdir()
        server = Server(Game([sys.executable, str(stand_in_game)], None, [preferences_dir], folder / "repo"))

        def call(tool, **arguments):
            answer = server.handle({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                    "params": {"name": tool, "arguments": arguments}})
            return answer["result"]

        def body(result):
            return json.loads(result["content"][0]["text"])

        def refusal(result):
            return result["content"][0]["text"] if result.get("isError") else ""

        init = server.handle({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                              "params": {"protocolVersion": "2025-03-26", "capabilities": {},
                                         "clientInfo": {"name": "self-test", "version": "0"}}})
        check(init["result"]["protocolVersion"] == "2025-03-26", "initialize keeps the client's revision")
        check(server.handle({"jsonrpc": "2.0", "method": "notifications/initialized"}) is None,
              "a notification is not answered")
        listed = server.handle({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})["result"]["tools"]
        check({tool["name"] for tool in listed} >= {"game_launch", "click_control", "screenshot"},
              "tools/list names the tools")
        check(server.handle({"jsonrpc": "2.0", "id": 3, "method": "nope"})["error"]["code"]
              == METHOD_NOT_FOUND, "an unknown method is refused")
        check(call("screen").get("isError"), "a tool without a game is refused")

        # The executables game_launch may start.
        check(find_executables(folder / "repo") == [own.resolve()],
              "find_executables picks this system's executable, not the newer one of the other system")
        bundle_binary = folder / "Open Annihilation.app" / "Contents" / "MacOS" / "open-annihilation"
        bundle_binary.parent.mkdir(parents=True)
        bundle_binary.write_bytes(b"")
        check(game_executable(folder / "Open Annihilation.app") == bundle_binary.resolve(),
              "a .app bundle names the binary in Contents/MacOS")
        stray = folder / "Open Annihilation.app" / "Contents" / "Resources" / "open-annihilation"
        stray.parent.mkdir()
        stray.write_bytes(b"")
        foreign = folder / "tool.sh"
        foreign.write_bytes(b"")
        for path, what in ((foreign, "a program of another name"), (stray, "a file elsewhere in a bundle")):
            try:
                game_executable(path)
                check(False, f"game_executable refuses {what}")
            except ToolError:
                pass
        try:
            Game(executable=foreign)
            check(False, "the server's --executable refuses a program of another name")
        except ToolError:
            pass
        elsewhere = folder / "elsewhere" / EXECUTABLE_NAME
        elsewhere.parent.mkdir()
        elsewhere.write_bytes(b"")
        check("not Open Annihilation's executable" in refusal(call("game_launch", executable=str(foreign))),
              "game_launch refuses a foreign executable")
        check("not one the server found" in refusal(call("game_launch", executable=str(elsewhere))),
              "game_launch refuses an executable the server did not find")
        check(call("game_launch", executable=str(folder / "none")).get("isError"),
              "game_launch refuses a missing executable")
        check(server.game.process is None, "a refused game_launch starts nothing")

        # Attaching, and the endpoint files refused.
        check(call("game_attach", file=str(stand_in.file)).get("isError") is None, "game_attach connects")
        check(server.game.pid == os.getpid(), "game_attach keeps the endpoint file's process id")
        bad_files = [("non-loopback", {"address": "192.0.2.10:%d" % stand_in.port}),
                     ("all-addresses", {"address": "0.0.0.0:%d" % stand_in.port}),
                     ("numeric-address", {"address": 1}), ("no-port", {"address": "127.0.0.1"}),
                     ("short-token", {"token": "00ff"}), ("version-2", {"version": 2}),
                     ("no-pid", {"pid": "1"})]
        for name, changes in bad_files:
            path = stand_in.write_file(folder / f"{name}.json", **changes)
            check(call("game_attach", file=str(path)).get("isError"), f"game_attach refuses the {name} file")
        for name in ("non-loopback", "all-addresses"):
            check("loopback" in refusal(call("game_attach", file=str(folder / f"{name}.json"))),
                  f"game_attach refuses the {name} file before connecting")
        not_json = folder / "notes.txt"
        not_json.write_text("address 127.0.0.1\n", encoding="utf-8")
        os.chmod(not_json, 0o600)
        check("not JSON" in refusal(call("game_attach", file=str(not_json))), "game_attach refuses a file not JSON")
        large = folder / "large.json"
        large.write_text(" " * MAX_ENDPOINT_FILE_BYTES + stand_in.description(), encoding="utf-8")
        os.chmod(large, 0o600)
        check(call("game_attach", file=str(large)).get("isError"), "game_attach refuses a large file")
        if hasattr(os, "getuid"):
            open_file = stand_in.write_file(folder / "open.json")
            os.chmod(open_file, 0o666)
            check(call("game_attach", file=str(open_file)).get("isError"),
                  "game_attach refuses a file others may write")
        localhost = stand_in.write_file(folder / "localhost.json", address="localhost:%d" % stand_in.port)
        check(call("game_attach", file=str(localhost)).get("isError") is None,
              "game_attach connects to localhost as 127.0.0.1")

        # The tools against the stand-in.
        check(body(call("screen"))["screen"] == "main_menu", "screen names the main menu")
        controls = body(call("controls", name_contains="single"))
        check(controls["count"] == 1 and controls["controls"][0]["name"] == "SINGLE",
              "controls filters by name or text")
        check(call("click_control", name="NOPE").get("isError"), "an unknown control is refused")
        chosen = body(call("select_list_item", list_name="MAPS", item="d.tnt"))
        check(chosen["wanted_index"] == 3 and stand_in.list_first == 2,
              "select_list_item scrolls the list and finds an item by its key")
        clicked = body(call("click_control", text="single player"))
        check(clicked["screen"] == "single_player", "click_control by text opens Single Player")
        last = stand_in.inputs[-1]
        check(last[1] == {"kind": "button_down", "x": 140, "y": 110, "space": "game", "button": "left",
                          "clicks": 1}, "the click lands at the control's centre")
        call("press_key", key="ctrl+s")
        check([event["kind"] for event in stand_in.inputs[-1]] == ["key_down", "key_down", "key_up", "key_up"]
              and stand_in.inputs[-1][0]["scancode"] == 224 and stand_in.inputs[-1][1]["scancode"] == 22,
              "press_key presses a chord in order and lets go in reverse")
        check(call("press_key", key="nokey").get("isError"), "an unknown key is refused")
        picture = call("screenshot")
        png = base64.b64decode(picture["content"][0]["data"])
        check(png.startswith(b"\x89PNG") and picture["content"][0]["mimeType"] == "image/png",
              "screenshot answers a PNG")
        check(body(call("subscribe", events="all"))["events"] == "all", "subscribe is answered")
        events = body(call("read_events", wait_seconds=2))["events"]
        check(len(events) == 1 and events[0]["event"] == "screen", "read_events gives the event sent")
        check(call("wait_for", screen="single_player", timeout=2).get("isError") is None,
              "wait_for sees the screen shown")
        check(call("wait_for", screen="match", timeout=0.3).get("isError"), "wait_for gives up in time")
        check(call("request", op="bogus").get("isError"), "request passes a refusal on")
        started = time.monotonic()
        answered = call("request", op="screen", fields={"id": 999999, "op": "quit", "timeout": 1e9})
        sent = stand_in.requests[-1]
        check(answered.get("isError") is None and sent["op"] == "screen" and sent["id"] != 999999
              and time.monotonic() - started < ANSWER_SECONDS,
              "request's fields cannot replace its id and op, nor make the server wait")
        check(call("click", x=140.0, y=110).get("isError") is None, "a whole number given as 140.0 is taken")

        # Arguments that do not meet a tool's schema are refused, and the server goes on.
        for tool, arguments, what in (
                ("screenshot", {"max_width": -1}, "a negative max_width"),
                ("screenshot", {"max_width": 0}, "a max_width of 0"),
                ("press_key", {"key": 5}, "a key that is a number"),
                ("press_key", {"key": "a", "repeat": 100, "hold_frames": 900}, "keys held too long"),
                ("select_list_item", {"list_name": "MAPS", "item": 5}, "an item that is a number"),
                ("select_list_item", {"list_name": "MAPS"}, "a missing item"),
                ("click", {"x": "1", "y": 2}, "a point given as text"),
                ("click", {"x": 1.5, "y": 2}, "a point that is not whole"),
                ("click", {"x": True, "y": 2}, "a point that is true"),
                ("drag", {"x1": 0, "y1": 0, "x2": 1, "y2": 1, "steps": 0}, "no steps"),
                ("wait_for", {"screen": "match", "timeout": float("nan")}, "a timeout that is not a number"),
                ("wait_for", {"screen": "match", "timeout": 1e9}, "a timeout too long"),
                ("read_events", {"wait_seconds": -1}, "a negative wait"),
                ("game_launch", {"arguments": "--mute"}, "arguments that are not a list"),
                ("game_launch", {"arguments": [5]}, "an argument that is a number"),
                ("request", {"op": 5}, "an op that is a number"),
                ("request", {"op": "screen", "fields": [1]}, "fields that are not an object"),
                ("subscribe", {"events": 5}, "events of neither form")):
            check(call(tool, **arguments).get("isError"), f"{tool} refuses {what}")
        broken = server.tools.room
        server.tools.room = lambda: 1 // 0
        failed = refusal(call("room"))
        server.tools.room = broken
        check("ZeroDivisionError" in failed and body(call("screen"))["screen"] == "single_player",
              "a tool's own failure is its error, and the server goes on")
        lines = [{"jsonrpc": "2.0", "id": 10, "method": "tools/call", "params": [1]},
                 {"jsonrpc": "2.0", "id": 11, "method": "tools/call",
                  "params": {"name": "screen", "arguments": [1]}},
                 {"jsonrpc": "2.0", "id": 12, "method": "tools/call",
                  "params": {"name": "press_key", "arguments": {"key": 5}}},
                 {"jsonrpc": "2.0", "id": 13, "method": "ping"}]
        written = io.StringIO()
        server.serve(io.StringIO("".join(json.dumps(line) + "\n" for line in lines)), written)
        answers = [json.loads(line) for line in written.getvalue().splitlines()]
        check([answer["id"] for answer in answers] == [10, 11, 12, 13]
              and answers[0]["error"]["code"] == INVALID_PARAMS and answers[2]["result"]["isError"]
              and answers[3]["result"] == {},
              "serve answers malformed calls and goes on")
        check("quit sent" in call("game_quit")["content"][0]["text"], "game_quit sends quit")

        # game_launch's checks, each made before anything starts.
        installation = folder / "installation"
        installation.mkdir()
        outside_preferences = folder / "outside.conf"
        outside_preferences.write_text(PREFERENCES_HEADER, encoding="utf-8")
        scratch = server.game.scratch
        for arguments, what in (
                (["--fark-address", "0.0.0.0:1"], "--fark-address"), (["--fark"], "--fark"),
                (["--fark-file", str(folder / "x.json")], "--fark-file"),
                (["--preferences-file", str(outside_preferences)], "--preferences-file"),
                (["--game-dir", str(installation)], "--game-dir"), (["--stage", "a.stage"], "--stage"),
                (["--headless-check"], "--headless-check"), (["german"], "a word that is no option"),
                (["--user-folder", str(folder)], "a folder outside the scratch folder"),
                (["--open", "../../file.oamod"], "a relative path out of the scratch folder"),
                (["--player-name"], "an option without its value"),
                (["--player-name", "--fark"], "an option as a value"),
                (["--skip-intro=yes"], "a value to a switch"),
                (["--hardware-acceleration=most"], "a word the option does not take"),
                (["--player-name", "a\0b"], "a zero character")):
            check(call("game_launch", arguments=arguments).get("isError"), f"game_launch refuses {what}")
        check("holds no totala1.hpi" in refusal(call("game_launch", game_dir=str(installation))),
              "game_launch refuses a game_dir without the game")
        check("outside" in refusal(call("game_launch", preferences_file=str(outside_preferences))),
              "game_launch refuses a preferences file outside the allowed folders")
        check(call("game_launch", preferences_file=str(preferences_dir / "none.conf")).get("isError"),
              "game_launch refuses a preferences file that does not exist")
        check(server.game.process is None and server.game.endpoint is None,
              "a refused game_launch starts nothing")

        # A launch through the stand-in game.
        (installation / "TotalA1.HPI").write_bytes(b"")
        taken = ["--resolution", "800x600", "--user-folder", "user", "--hardware-acceleration=off",
                 "--game-password", "secret"]
        launched = call("game_launch", game_dir=str(installation), headless=True, timeout=20, arguments=taken)
        check(launched.get("isError") is None, f"game_launch starts the game: {refusal(launched)}")
        if launched.get("isError") is None:
            command = json.loads(command_file.read_text(encoding="utf-8"))
            value = {option: command[index + 1] for index, option in enumerate(command[:-1])}
            preferences = Path(value.get("--preferences-file", ""))
            check(command[0] == str(own.resolve()), "game_launch starts the executable the server found")
            check(body(launched)["preferences_file"] == str(preferences), "game_launch names the preferences file")
            check(inside(preferences, scratch) and preferences.read_text(encoding="utf-8") == PREFERENCES_HEADER,
                  "the game gets a new preferences file in the scratch folder")
            check(inside(value.get("--fark-file", ""), scratch), "the endpoint file is in the scratch folder")
            check(value.get("--game-dir") == str(installation.resolve()), "game_dir is passed on")
            check(command[-len(taken):] == taken and "--skip-intro" in command,
                  "the arguments taken are passed on in order")
            check("refused" not in refusal(call("screen")) and body(call("screen"))["screen"],
                  "the launched game's endpoint answers")
            check("game_quit" in refusal(call("game_attach", file=str(stand_in.file))),
                  "game_attach is refused while the launched game runs")
            status = body(call("game_status"))
            check(status["launched_here"] and status["running"] and status["scratch"] == str(scratch)
                  and status["executables"] == [str(own.resolve())],
                  "game_status names the launched game, the scratch folder and the executables")
            check("stopped" in call("game_quit", force=True)["content"][0]["text"], "game_quit force stops it")
            check(call("game_attach", file=str(stand_in.file)).get("isError") is None
                  and not body(call("game_status"))["launched_here"] and server.game.pid == os.getpid(),
                  "game_attach after the launched game ended forgets it")
        own_preferences = preferences_dir / "mine.conf"
        own_preferences.write_text(PREFERENCES_HEADER, encoding="utf-8")
        launched = call("game_launch", preferences_file=str(own_preferences), timeout=20)
        check(launched.get("isError") is None and str(own_preferences.resolve())
              in json.loads(command_file.read_text(encoding="utf-8")),
              "game_launch takes a preferences file in a folder of --preferences-dir")
        call("game_quit", force=True)
        with_png, width, height = png_from_rgb(9, 3, bytes(81), 4)
        check((width, height) == (3, 1) and with_png.startswith(b"\x89PNG"), "a picture shrinks by steps")
        server.shutdown()
        check(not scratch.exists(), "the scratch folder is removed when the server ends")
    finally:
        if server is not None:
            server.shutdown()
        os.environ.update(kept_environment)
        shutil.rmtree(folder, ignore_errors=True)
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    print("oa-mcp self-test: " + ("passed" if not failures else f"{len(failures)} failed"))
    return 1 if failures else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--self-test", action="store_true",
                        help="check the server against a stand-in endpoint and exit")
    parser.add_argument("--runner", default="",
                        help="a command game_launch starts the game through, such as a compatibility layer")
    parser.add_argument("--executable", type=Path,
                        help="the game's executable (open-annihilation, open-annihilation.exe or a .app "
                             "bundle) game_launch starts; by default OA_EXECUTABLE or the newest build")
    parser.add_argument("--preferences-dir", type=Path, action="append", default=[],
                        help="a folder whose preferences files game_launch may give the game (again for more)")
    options = parser.parse_args(argv)
    if options.self_test:
        return self_test()
    try:
        game = Game(shlex.split(options.runner), options.executable, options.preferences_dir)
    except ToolError as error:
        parser.error(f"--executable: {error}")
    stdin = open(sys.stdin.fileno(), "r", encoding="utf-8", newline="\n", closefd=False)
    stdout = open(sys.stdout.fileno(), "w", encoding="utf-8", newline="\n", closefd=False)
    server = Server(game)
    try:
        server.serve(stdin, stdout)
    finally:
        server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
