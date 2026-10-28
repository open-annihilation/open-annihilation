#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""A skirmish reports its status on standard output and then leaves.

The game runs through its main loop on the dummy SDL drivers, started with
--status-stdout, --quit-after-game, --classic-main-menu and --fark.
Standard output and standard error are read apart. Every line of standard output is one JSON object:
hello first, then a screen, the match starting, game_over with each
player's result, and exit last with reason game-ended and code 0. The
process exits 0. The log goes to a scratch folder.

The endpoint is only how the check drives the game. It clicks Single
Player, Skirmish and Start, then types the developer passphrase and
+IWin. Typed text is taken only while the chat line is open, so each
line is three requests: Return opens the line, the text follows, and
Return sends it. Each request is answered once the game has taken it.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import threading
import time

# The helpers live with the other native checks. This script does not use
# that file's Game, which merges standard error into standard output.
_TOOLS = Path(__file__).resolve().parents[4] / "tools"
sys.path.insert(0, str(_TOOLS))
import check_native_automation as automation  # noqa: E402

AutomationFailure = automation.AutomationFailure
Client = automation.Client
click_control = automation.click_control
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
# Longer than the imported checks' 120s: loading a skirmish can take that.
MATCH_TIMEOUT = 600
STEP_TIMEOUT = 180
# The win is the next economy checks, then the score screen, then the quit.
END_TIMEOUT = 90
# SDL's Return: scancode 40, key code 13.
RETURN_SCAN = 40
RETURN_KEY = 13
PASSPHRASE = "+Now Film Chris Include Reload Assert"
WIN = "+IWin"


def return_key(down):
    """Return pressed or released, as a keyboard sends it."""
    return {"kind": "key_down" if down else "key_up", "key": "Return",
            "scancode": RETURN_SCAN, "keycode": RETURN_KEY}


def taken(client, events, what):
    """Sends input and raises unless the game took it."""
    answer = client.request("input", expect_screen="match", events=events)
    if not answer.get("ok") or not isinstance(answer.get("consumed", {}).get("frame"), int):
        raise AutomationFailure(f"{what} was answered {answer}")
    return answer


def say(client, text):
    """Opens the chat line, types one line and sends it.

    A text event is delivered only while the game is taking text. The
    chat line starts that when Return opens it, so the text is its own
    request, after that Return has been taken.
    """
    taken(client, [return_key(True), return_key(False)], "opening the chat line")
    taken(client, [{"kind": "text", "text": text}], f"typing {text}")
    taken(client, [return_key(True), return_key(False)], f"sending {text}")


def wait_screen(client, name, timeout):
    """Waits for the screen named and returns screen's answer."""
    deadline = time.monotonic() + timeout
    screen = client.request("screen")
    while screen.get("screen") != name and time.monotonic() < deadline:
        time.sleep(0.1)
        screen = client.request("screen")
    if screen.get("screen") != name:
        raise AutomationFailure(f"the screen is {screen.get('screen')}, not {name}: {screen}")
    return screen


def read_pipe(pipe, into):
    """Reads a pipe to the end, keeping each line."""
    for line in pipe:
        into.append(line)


class Game:
    """The running game. Standard output and standard error stay apart."""

    def __init__(self, native, game_dir, workdir):
        self.stdout_lines = []
        self.stderr_lines = []
        self.preferences = workdir / "automation.conf"
        self.preferences.write_text(automation.PREFERENCES, encoding="utf-8")
        self.endpoint_file = workdir / "endpoint" / "automation.json"
        self.log_dir = workdir / "logs"
        self.user_folder = workdir / "user"
        self.log_dir.mkdir()
        self.user_folder.mkdir()
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        self.process = subprocess.Popen(
            [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
             "--status-stdout", "--quit-after-game", "--classic-main-menu", "--fark", "--fark-file",
             str(self.endpoint_file), "--preferences-file", str(self.preferences),
             "--log-dir", str(self.log_dir), "--user-folder", str(self.user_folder)],
            cwd=workdir, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, errors="replace")
        self._out = threading.Thread(
            target=read_pipe, args=(self.process.stdout, self.stdout_lines), daemon=True)
        self._err = threading.Thread(
            target=read_pipe, args=(self.process.stderr, self.stderr_lines), daemon=True)
        self._out.start()
        self._err.start()

    def endpoint(self):
        """Waits for the endpoint's file and returns what it holds."""
        deadline = time.monotonic() + STEP_TIMEOUT
        while time.monotonic() < deadline:
            if self.endpoint_file.is_file():
                return json.loads(self.endpoint_file.read_text(encoding="utf-8"))
            if self.process.poll() is not None:
                break
            time.sleep(0.1)
        raise AutomationFailure("the game wrote no endpoint file")

    def wait_end(self):
        try:
            status = self.process.wait(timeout=END_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise AutomationFailure(f"the game did not end within {END_TIMEOUT} s") from None
        self._out.join(timeout=5)
        self._err.join(timeout=5)
        return status

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        self._out.join(timeout=5)
        self._err.join(timeout=5)


def objects(lines):
    """Parses each line as one JSON object."""
    parsed = []
    for line in lines:
        text = line.strip()
        if not text:
            continue
        try:
            value = json.loads(text)
        except json.JSONDecodeError as error:
            raise AutomationFailure(f"a line is not JSON: {text!r}") from error
        if not isinstance(value, dict):
            raise AutomationFailure(f"a line is not an object: {text!r}")
        parsed.append(value)
    return parsed


def check(native, game_dir, workdir):
    """Drives a skirmish to its end and checks the status lines."""
    automation.TIMEOUT = MATCH_TIMEOUT
    game = Game(native, game_dir, workdir)
    client = None
    try:
        endpoint = game.endpoint()
        address, token = endpoint.get("address", ""), endpoint.get("token", "")
        if not address.startswith("127.0.0.1:") or len(token) != 32:
            raise AutomationFailure(f"the endpoint file holds {endpoint}")
        client = Client(address)
        hello = client.request("hello", versions=[1], client="status", token=token)
        if not hello.get("ok"):
            raise AutomationFailure(f"hello was answered {hello}")
        wait_screen(client, "main_menu", STEP_TIMEOUT)
        click_control(client, "SINGLE", "main_menu")
        wait_screen(client, "single_player", STEP_TIMEOUT)
        click_control(client, "Skirmish", "single_player")
        wait_screen(client, "skirmish", STEP_TIMEOUT)
        click_control(client, "Start", "skirmish")
        wait_screen(client, "match", MATCH_TIMEOUT)
        say(client, PASSPHRASE)
        say(client, WIN)
        status = game.wait_end()
        lines = objects(game.stdout_lines)
        if not lines:
            raise AutomationFailure(
                "standard output was empty; standard error: "
                + "".join(game.stderr_lines[-20:]))
        if lines[0].get("event") != "hello" or lines[0].get("status") != 1 or \
                not isinstance(lines[0].get("version"), str) or not lines[0].get("version") or \
                not isinstance(lines[0].get("pid"), int):
            raise AutomationFailure(f"the first line is {lines[0]}")
        if not any(line.get("event") == "screen" for line in lines):
            raise AutomationFailure("no screen line")
        if not any(line.get("event") == "match" and line.get("change") == "started"
                   for line in lines):
            raise AutomationFailure("the match did not start")
        ended = [line for line in lines if line.get("event") == "game_over"]
        if len(ended) != 1 or not isinstance(ended[0].get("results"), list) or \
                not ended[0]["results"]:
            raise AutomationFailure(f"game_over is {ended}")
        last = lines[-1]
        if last.get("event") != "exit" or last.get("reason") != "game-ended" or \
                last.get("code") != 0:
            raise AutomationFailure(f"the last line is {last}")
        if status != 0:
            raise AutomationFailure(f"the game exited with {status}")
    except (AutomationFailure, OSError, ValueError) as failure:
        game.stop()
        print("stdout:")
        print("".join(game.stdout_lines))
        print("stderr:")
        print("".join(game.stderr_lines))
        if isinstance(failure, AutomationFailure):
            raise
        raise AutomationFailure(f"{type(failure).__name__}: {failure}") from failure
    finally:
        if client is not None:
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
            print(f"native-status-stdout: {failure}", file=sys.stderr)
            return 1
    print("native-status-stdout: the status lines named the game and its end")
    return 0


if __name__ == "__main__":
    sys.exit(main())
