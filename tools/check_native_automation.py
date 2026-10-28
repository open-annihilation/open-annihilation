#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the automation endpoint of a running game, end to end.

The game runs through its main loop on the dummy SDL drivers, started with
--fark, --fark-file and --classic-main-menu: this check and those that
start the game through it read the game's own main menu, MAINMENU.GUI. The
check reads the address and token from the file, which only its owner may
read, and then, over 127.0.0.1:

- a connection that speaks no frames is closed unanswered, and so is one
  whose first frame is larger than a hello may be;
- a hello with a wrong token is answered denied, and the connection closed;
- a hello with the token is answered with the protocol's and the engine's
  versions, the window's size and the canvas's place in it;
- while that client is connected, a second one is answered busy;
- screen names the main menu; prefs gives the preferences file and the
  values it held, all of them or those named; an unknown operation and a
  request without an op are refused;
- controls names the main menu's SINGLE, MULTI and EXIT buttons with their
  places on the canvas and in the window; input that expects another
  screen is refused screen_changed and pushes nothing, events that cannot
  be read are refused bad_request, and a click on SINGLE through the event
  queue is answered once the game has taken it and opens Single Player;
- Load Game, with no saved games, stacks its message box over its dialog:
  screen lists msgbox before loadgame, and controls the box's OK first;
  OK closes the box and Cancel the dialog;
- a client that sends hello and quit at once and then ends its side of the
  connection has both answered, and the game ends with status 0.

The log says where the endpoint serves and never holds the token.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import shlex
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import zlib

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Seconds the game may take to start serving, to answer, or to end.
TIMEOUT = 120
# What the preferences file holds before the run.
STORED_ADDRESS = "192.0.2.7"
PREFERENCES = ('open-annihilation-preferences 1\n'
               f'"multiplayer|tcpaddr" "{STORED_ADDRESS}"\n')
MAGIC = b"AUTO/1 "
# The longest JSON part a connection's first frame may have, in bytes.
HELLO_JSON_BYTES = 4096


class AutomationFailure(Exception):
    """A behaviour that differs from the expected one."""


def encode(message):
    """One frame of the automation protocol, route '-', holding a JSON object."""
    body = json.dumps(message, separators=(",", ":")).encode()
    return b"AUTO/1 - %d 0 %08x\n" % (len(body), zlib.crc32(body)) + body


class Client:
    """One connection to the endpoint, which reads its frames as they come."""

    def __init__(self, address):
        host, _, port = address.rpartition(":")
        family = socket.AF_INET6 if host.startswith("[") else socket.AF_INET
        self.sock = socket.socket(family, socket.SOCK_STREAM)
        self.sock.settimeout(TIMEOUT)
        self.sock.connect((host.strip("[]"), int(port)))
        self.buffer = b""
        self.next_id = 0

    def send_raw(self, data):
        self.sock.sendall(data)

    def read_frame(self):
        """Returns the next frame's JSON object and payload; None when the endpoint closed the connection."""
        while True:
            if self.buffer and not MAGIC.startswith(self.buffer[:len(MAGIC)]) \
                    and not self.buffer.startswith(MAGIC):
                raise AutomationFailure(f"the endpoint sent bytes that are no frame: {self.buffer[:40]!r}")
            line_end = self.buffer.find(b"\n")
            if line_end >= 0:
                fields = self.buffer[:line_end].decode("ascii").split(" ")
                if len(fields) != 5 or fields[1] != "-":
                    raise AutomationFailure(f"a malformed header: {self.buffer[:line_end]!r}")
                json_length, payload_length = int(fields[2]), int(fields[3])
                total = line_end + 1 + json_length + payload_length
                if len(self.buffer) >= total:
                    body = self.buffer[line_end + 1:line_end + 1 + json_length]
                    payload = self.buffer[line_end + 1 + json_length:total]
                    self.buffer = self.buffer[total:]
                    if f"{zlib.crc32(body + payload):08x}" != fields[4]:
                        raise AutomationFailure("a frame whose CRC does not match")
                    return json.loads(body), payload
            chunk = self.sock.recv(65536)
            if not chunk:
                if self.buffer:
                    raise AutomationFailure("the endpoint closed the connection inside a frame")
                return None
            self.buffer += chunk

    def request(self, op, **fields):
        """Sends a request and returns its answer."""
        self.next_id += 1
        self.send_raw(encode({"id": self.next_id, "op": op, **fields}))
        answer = self.read_frame()
        if answer is None:
            raise AutomationFailure(f"the endpoint closed the connection instead of answering {op}")
        message, _ = answer
        if message.get("id") != self.next_id:
            raise AutomationFailure(f"the answer to {op} names another request: {message}")
        return message

    def expect_closed(self, what):
        """Raises AutomationFailure unless the endpoint closes the connection without sending more."""
        try:
            if self.read_frame() is not None:
                raise AutomationFailure(f"the endpoint answered again after {what}")
        except ConnectionResetError:
            # Closed with bytes it had not read: the system resets the connection.
            pass

    def close(self):
        self.sock.close()


def refused(message, code):
    return message.get("ok") is False and message.get("error", {}).get("code") == code


def find_control(client, name):
    """Returns the shown screen's control of a name, compared ignoring case."""
    answer = client.request("controls")
    for control in answer.get("controls", []):
        if control.get("name", "").lower() == name.lower():
            return control
    raise AutomationFailure(f"{answer.get('screen')} has no control {name}: {answer}")


def click_events(x, y, space="game"):
    """The events of a left click at a point, as a mouse makes them."""
    return [{"kind": "pointer_move", "x": x, "y": y, "space": space},
            {"kind": "button_down", "x": x, "y": y, "space": space, "button": "left", "clicks": 1},
            {"kind": "button_up", "x": x, "y": y, "space": space, "button": "left", "clicks": 1}]


def click_control(client, name, screen):
    """Clicks a control at its centre on the screen named, and returns the answer."""
    x, y, width, height = find_control(client, name)["rect"]
    answer = client.request("input", expect_screen=screen,
                            events=click_events(x + width // 2, y + height // 2))
    if not answer.get("ok") or not isinstance(answer.get("consumed", {}).get("frame"), int):
        raise AutomationFailure(f"the click on {name} was answered {answer}")
    return answer


def wait_dialogs(client, dialogs):
    """Waits for the dialogs named over the screen, the top one first, and returns screen's answer."""
    deadline = time.monotonic() + TIMEOUT
    screen = client.request("screen")
    while screen.get("dialogs") != dialogs and time.monotonic() < deadline:
        time.sleep(0.1)
        screen = client.request("screen")
    if screen.get("dialogs") != dialogs:
        raise AutomationFailure(f"the dialogs are {screen.get('dialogs')}, not {dialogs}: {screen}")
    return screen


def wait_screen(client, name):
    """Waits for the screen named and returns screen's answer."""
    deadline = time.monotonic() + TIMEOUT
    screen = client.request("screen")
    while screen.get("screen") != name and time.monotonic() < deadline:
        time.sleep(0.1)
        screen = client.request("screen")
    if screen.get("screen") != name:
        raise AutomationFailure(f"the screen is {screen.get('screen')}, not {name}: {screen}")
    return screen


class Game:
    """The running game, whose output the check keeps; arguments are added to its command line."""

    def __init__(self, native, game_dir, workdir, arguments=()):
        self.lines = []
        self._queue = queue.Queue()
        self.preferences = workdir / "automation.conf"
        self.preferences.write_text(PREFERENCES, encoding="utf-8")
        self.file = workdir / "endpoint" / "automation.json"
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.process = subprocess.Popen(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
             "--preferences-file", str(self.preferences), "--fark", "--fark-file", str(self.file),
             "--classic-main-menu", *arguments],
            cwd=workdir, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def take_output(self):
        while not self._queue.empty():
            line = self._queue.get()
            if line is not None:
                self.lines.append(line)

    def endpoint(self):
        """Waits for the endpoint's file and returns what it holds."""
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            if self.file.is_file():
                return json.loads(self.file.read_text(encoding="utf-8"))
            if self.process.poll() is not None:
                break
            time.sleep(0.1)
        raise AutomationFailure("the game wrote no endpoint file")

    def wait_end(self):
        try:
            status = self.process.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise AutomationFailure(f"the game did not end within {TIMEOUT} s of quit") from None
        time.sleep(0.2)
        self.take_output()
        if status != 0:
            raise AutomationFailure(f"the game exited with {status}")

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()


def check(native, game_dir, workdir):
    """Drives the endpoint; raises AutomationFailure when it differs."""
    game = Game(native, game_dir, workdir)
    clients = []
    try:
        endpoint = game.endpoint()
        address, token = endpoint.get("address", ""), endpoint.get("token", "")
        if endpoint.get("version") != 1 or not address.startswith("127.0.0.1:") or \
                len(token) != 32 or not RUNNER and endpoint.get("pid") != game.process.pid:
            raise AutomationFailure(f"the endpoint file holds {endpoint}")
        if os.name == "posix" and stat.S_IMODE(game.file.stat().st_mode) != 0o600:
            raise AutomationFailure(f"the endpoint file's mode is {oct(game.file.stat().st_mode)}")

        stranger = Client(address)
        clients.append(stranger)
        stranger.send_raw(b"GET / HTTP/1.0\r\n\r\n")
        if stranger.sock.recv(1) != b"":
            raise AutomationFailure("a connection that speaks no frames was answered")

        # A first frame's JSON is read before its token is checked, so it may
        # be no larger than a hello needs.
        oversized = Client(address)
        clients.append(oversized)
        oversized.send_raw(encode({"id": 1, "op": "hello", "versions": [1],
                                   "client": "x" * HELLO_JSON_BYTES, "token": token}))
        oversized.expect_closed("a first frame larger than a hello")

        guesser = Client(address)
        clients.append(guesser)
        guesser.next_id = 40
        denied = guesser.request("hello", versions=[1], client="check", token="0" * 32)
        if not refused(denied, "denied"):
            raise AutomationFailure(f"a wrong token was answered {denied}")
        guesser.expect_closed("denied")

        client = Client(address)
        clients.append(client)
        hello = client.request("hello", versions=[1], client="check", token=token)
        if not hello.get("ok") or hello.get("version") != 1 or hello.get("automation") != 1 or \
                hello.get("tick_rate") != 30 or not hello.get("engine", {}).get("version"):
            raise AutomationFailure(f"hello was answered {hello}")
        window, canvas = hello.get("window", {}), hello.get("canvas", {})
        rect = canvas.get("rect", [])
        if window.get("width", 0) <= 0 or window.get("height", 0) <= 0 or \
                (canvas.get("width"), canvas.get("height")) != (640, 480) or len(rect) != 4 or \
                rect[0] + rect[2] > window["width"] or rect[1] + rect[3] > window["height"]:
            raise AutomationFailure(f"hello gave the window {window} and the canvas {canvas}")

        second = Client(address)
        clients.append(second)
        second.next_id = 70
        busy = second.request("hello", versions=[1], client="second", token=token)
        if not refused(busy, "busy"):
            raise AutomationFailure(f"a second client was answered {busy}")
        second.expect_closed("busy")

        deadline = time.monotonic() + TIMEOUT
        screen = client.request("screen")
        while screen.get("screen") != "main_menu" and time.monotonic() < deadline:
            time.sleep(0.1)
            screen = client.request("screen")
        if screen.get("screen") != "main_menu" or not isinstance(screen.get("frontend_state"), int) or \
                screen.get("dialogs") != [] or "pointer" not in screen:
            raise AutomationFailure(f"screen was answered {screen}")
        if not isinstance(screen.get("frame"), int) or not isinstance(screen.get("tick"), int):
            raise AutomationFailure(f"screen's answer gives no frame and tick: {screen}")

        prefs = client.request("prefs")
        if Path(prefs.get("path", "")).resolve() != game.preferences.resolve() or \
                prefs.get("values", {}).get("multiplayer|tcpaddr") != STORED_ADDRESS:
            raise AutomationFailure(f"prefs was answered {prefs}")
        named = client.request("prefs", names=["multiplayer|tcpaddr"])
        if named.get("values") != {"multiplayer|tcpaddr": STORED_ADDRESS}:
            raise AutomationFailure(f"prefs for one name was answered {named}")
        if not refused(client.request("prefs", names="multiplayer|tcpaddr"), "bad_request"):
            raise AutomationFailure("prefs with names that are not a list was not refused")
        if not refused(client.request("dance"), "unknown_op"):
            raise AutomationFailure("an unknown operation was not refused")
        client.next_id += 1
        client.send_raw(encode({"id": client.next_id}))
        answer, _ = client.read_frame()
        if not refused(answer, "bad_request") or answer.get("error", {}).get("field") != "op":
            raise AutomationFailure(f"a request without an op was answered {answer}")

        menu = client.request("controls")
        named = {control["name"]: control for control in menu.get("controls", [])}
        for name in ("SINGLE", "MULTI", "EXIT"):
            control = named.get(name)
            if control is None or control.get("kind") != "button" or not control.get("enabled") or \
                    control.get("dialog") is not None:
                raise AutomationFailure(f"the main menu's {name} is {control}: {menu}")
            x, y, width, height = control["rect"]
            wx, wy, wwidth, wheight = control["window_rect"]
            if width <= 0 or height <= 0 or x < 0 or y < 0 or x + width > 640 or y + height > 480 or \
                    not rect[0] <= wx < rect[0] + rect[2] or not rect[1] <= wy < rect[1] + rect[3] or \
                    wwidth < width or wheight < height:
                raise AutomationFailure(f"the main menu's {name} lies at {control}")
        x, y, width, height = named["SINGLE"]["rect"]
        elsewhere = client.request("input", expect_screen="single_player",
                                   events=click_events(x + width // 2, y + height // 2))
        if not refused(elsewhere, "screen_changed"):
            raise AutomationFailure(f"input for another screen was answered {elsewhere}")
        if not refused(client.request("input", events={"kind": "text"}), "bad_request") or \
                not refused(client.request("input", events=[{"kind": "fly"}]), "bad_request"):
            raise AutomationFailure("input with events that cannot be read was not refused")
        still = client.request("screen")
        if still.get("screen") != "main_menu":
            raise AutomationFailure("refused input reached the game")
        # Events pushed at one frame's pump are taken by the next frame's
        # poll, and the answer follows at that frame's pump.
        click = click_control(client, "SINGLE", "main_menu")
        if click["consumed"]["frame"] < still["frame"] + 2 or click["consumed"]["tick"] != 0:
            raise AutomationFailure(f"the click on SINGLE was answered {click} after {still}")
        single = wait_screen(client, "single_player")
        if single.get("dialogs") != []:
            raise AutomationFailure(f"Single Player was answered {single}")

        # The check's own folder holds no saved games, so Load Game says so
        # in a message box over its dialog, which takes the pointer first.
        click_control(client, "LoadGame", "single_player")
        wait_screen(client, "load_game")
        wait_dialogs(client, ["msgbox", "loadgame"])
        listed = client.request("controls").get("controls", [])
        box = [control for control in listed if control.get("dialog") == "msgbox"]
        if not box or box != listed[:len(box)] or box[0].get("name") != "OK" or \
                not box[0].get("enabled") or \
                "There are no saved games to choose from" not in [c.get("text") for c in box]:
            raise AutomationFailure(f"Load Game's message box is listed as {listed}")
        click_control(client, "OK", "load_game")
        wait_dialogs(client, ["loadgame"])
        click_control(client, "CANCEL", "load_game")
        back = wait_screen(client, "single_player")
        if back.get("dialogs") != []:
            raise AutomationFailure(f"Single Player after Load Game was answered {back}")

        # A one-shot client sends its requests and ends its side of the
        # connection: each is answered all the same. Until the endpoint has
        # seen the last client go, a new one is busy.
        client.close()
        deadline = time.monotonic() + TIMEOUT
        while True:
            last = Client(address)
            clients.append(last)
            last.send_raw(encode({"id": 1, "op": "hello", "versions": [1], "client": "one-shot",
                                  "token": token}) + encode({"id": 2, "op": "quit"}))
            last.sock.shutdown(socket.SHUT_WR)
            answer = last.read_frame()
            if answer is None:
                raise AutomationFailure("a client that ended its side after hello and quit was not answered")
            if not refused(answer[0], "busy") or time.monotonic() > deadline:
                break
            last.expect_closed("busy")
            time.sleep(0.1)
        if not answer[0].get("ok") or answer[0].get("id") != 1:
            raise AutomationFailure(f"the one-shot client's hello was answered {answer[0]}")
        quit_answer = last.read_frame()
        if quit_answer is None or not quit_answer[0].get("ok") or quit_answer[0].get("id") != 2:
            raise AutomationFailure(f"the one-shot client's quit was answered {quit_answer}")
        last.expect_closed("quit")
        game.wait_end()
        game.take_output()
        output = "\n".join(game.lines)
        for line in (f"automation: serving on {address}", "automation: client connected"):
            if line not in game.lines:
                raise AutomationFailure(f"the log does not say: {line}")
        if token in output:
            raise AutomationFailure("the log holds the token")
    except (AutomationFailure, OSError, ValueError) as failure:
        game.stop()
        game.take_output()
        print("\n".join(f"game: {line}" for line in game.lines))
        if isinstance(failure, AutomationFailure):
            raise
        raise AutomationFailure(f"{type(failure).__name__}: {failure}") from failure
    finally:
        for client in clients:
            client.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
        try:
            check(args.native, args.game_dir, Path(scratch))
        except AutomationFailure as failure:
            print(f"native-automation-menus: {failure}", file=sys.stderr)
            return 1
    print("native-automation-menus: the endpoint answered as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
