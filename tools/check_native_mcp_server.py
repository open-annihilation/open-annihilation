#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the MCP server (tools/mcp/oa_mcp_server.py) against the running game.

The server is started as an MCP client starts it, speaking MCP over its
standard input and output, with --executable naming the game under test.
Through its tools it starts the game on the dummy SDL drivers, and then:

- game_launch answers the endpoint's hello, and the game was given a new
  preferences file in the server's scratch folder; wait_for sees the main
  menu;
- controls lists the main menu's SINGLE button, and screenshot answers a PNG
  picture of the presented frame;
- after subscribe, click_control on SINGLE opens Single Player, and Escape
  through press_key goes back to the main menu;
- read_events gives the screen events of both changes, in order;
- game_quit ends the game with status 0, and game_status says so.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = os.environ.get("OA_TEST_RUNNER", "")
SERVER = Path(__file__).resolve().parent / "mcp" / "oa_mcp_server.py"
# Seconds the game may take to start and show its main menu.
START_SECONDS = 120
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class McpFailure(Exception):
    """The server or the game did not answer as the check expects."""


class Client:
    """The MCP server, started as a client starts it."""

    def __init__(self, native):
        self.process = subprocess.Popen(
            [sys.executable, str(SERVER), "--executable", str(native),
             *(["--runner", RUNNER] if RUNNER else [])],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
        self.next_id = 0

    def rpc(self, method, params=None):
        self.next_id += 1
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.next_id, "method": method,
                                             "params": params or {}}) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise McpFailure(f"the server ended while answering {method}")
        answer = json.loads(line)
        if "error" in answer:
            raise McpFailure(f"{method}: {answer['error']}")
        return answer["result"]

    def tool(self, tool_name, **arguments):
        """Calls a tool; returns its content, and fails when the tool reports an error."""
        result = self.rpc("tools/call", {"name": tool_name, "arguments": arguments})
        if result.get("isError"):
            raise McpFailure(f"{tool_name}: {result['content'][0].get('text')}")
        return result["content"]

    def tool_json(self, tool_name, **arguments):
        return json.loads(self.tool(tool_name, **arguments)[0]["text"])

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.process.kill()


def check(native, game_dir):
    client = Client(native)
    try:
        info = client.rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                         "clientInfo": {"name": "native-mcp-server", "version": "1"}})
        if info["serverInfo"]["name"] != "open-annihilation":
            raise McpFailure(f"initialize named {info['serverInfo']}")
        launched = client.tool_json("game_launch", game_dir=str(game_dir), headless=True, mute=True,
                                    timeout=START_SECONDS)
        if launched["hello"].get("automation") != 1:
            raise McpFailure(f"game_launch answered {launched['hello']}")
        scratch = Path(client.tool_json("game_status")["scratch"])
        preferences = Path(launched["preferences_file"])
        if scratch not in preferences.parents or not preferences.is_file():
            raise McpFailure(f"the game was given no new preferences file in {scratch}: {launched}")
        client.tool_json("wait_for", screen="main_menu", timeout=START_SECONDS)
        names = [control["name"] for control in client.tool_json("controls")["controls"]]
        if "SINGLE" not in names:
            raise McpFailure(f"controls listed no SINGLE: {names}")
        picture = client.tool("screenshot")[0]
        if picture.get("type") != "image" or not base64.b64decode(picture["data"]).startswith(PNG_SIGNATURE):
            raise McpFailure("screenshot answered no PNG picture")
        client.tool_json("subscribe", events=["screen"])
        clicked = client.tool_json("click_control", name="SINGLE")
        client.tool_json("wait_for", screen="single_player", timeout=30)
        client.tool_json("press_key", key="escape")
        client.tool_json("wait_for", screen="main_menu", timeout=30)
        changes = [(event.get("previous"), event.get("name"))
                   for event in client.tool_json("read_events", wait_seconds=5)["events"]
                   if event.get("event") == "screen" and event.get("previous")]
        if changes != [("main_menu", "single_player"), ("single_player", "main_menu")]:
            raise McpFailure(f"read_events gave the screen changes {changes} (click: {clicked})")
        ended = client.tool("game_quit")[0]["text"]
        if ended != "the game ended with status 0":
            raise McpFailure(f"game_quit: {ended}")
        status = client.tool_json("game_status")
        if status["running"] or status["exit_code"] != 0:
            raise McpFailure(f"game_status after quit: {status}")
    finally:
        client.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", required=True, type=Path, help="the game's executable")
    parser.add_argument("--game-dir", required=True, type=Path, help="the installed game")
    parser.add_argument("--scratch-root", type=Path, help="where the check's own folder is made")
    args = parser.parse_args(argv)
    if args.scratch_root:
        args.scratch_root.mkdir(parents=True, exist_ok=True)
    # The server makes its own scratch folder in the system's temporary
    # folder; the check's own is where that is made.
    with tempfile.TemporaryDirectory(dir=args.scratch_root) as scratch:
        os.environ["TMPDIR"] = scratch
        try:
            check(args.native, args.game_dir)
        except McpFailure as failure:
            print(f"native-mcp-server: {failure}", file=sys.stderr)
            return 1
    print("native-mcp-server: the MCP server drove the game as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
