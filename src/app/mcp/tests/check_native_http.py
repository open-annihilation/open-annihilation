#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""The game speaks MCP over loopback HTTP, then leaves.

The game runs on the dummy SDL drivers, started with --mcp-http and not
--mcp. This client reads mcp.json beside the preferences, initializes,
calls game.status and game.quit, and waits for the process to exit 0.
The log and the preferences go to a scratch folder. The token is not printed.
"""
import argparse
import http.client
import json
import os
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

_TOOLS = Path(__file__).resolve().parents[4] / "tools"
sys.path.insert(0, str(_TOOLS))
import check_native_automation as automation  # noqa: E402

RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
START_TIMEOUT = 120
STEP_TIMEOUT = 30
END_TIMEOUT = 60


class Failure(Exception):
    """The native check failed."""


def require(condition, message):
    """Raises Failure when a condition does not hold."""
    if not condition:
        raise Failure(message)


def wait_endpoint(path, timeout):
    """Returns mcp.json once it is a complete object. The token is not printed."""
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
    raise Failure("mcp.json did not appear")


def message_of(content_type, payload):
    """Returns the JSON-RPC messages in a response body."""
    text = payload.decode("utf-8", "replace")
    if "text/event-stream" in content_type:
        messages = []
        for line in text.splitlines():
            if line.startswith("data:"):
                value = line[5:].lstrip()
                if value:
                    messages.append(json.loads(value))
        return messages
    if not text:
        return []
    return [json.loads(text)]


def post(host, port, token, body, session_id=None):
    """Posts one JSON-RPC message and returns the status, session id and messages."""
    connection = http.client.HTTPConnection(host, port, timeout=STEP_TIMEOUT)
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
    try:
        messages = message_of(content_type, payload)
    except json.JSONDecodeError as error:
        raise Failure(f"the response was not JSON (HTTP {status})") from error
    return status, session, messages


def main():
    """Runs the check."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True)
    parser.add_argument("--game-dir", required=True)
    parser.add_argument("--scratch-root", required=True)
    args = parser.parse_args()
    root = Path(args.scratch_root)
    root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="M03-mcp-", dir=root) as work:
        workdir = Path(work)
        preferences = workdir / "mcp.conf"
        preferences.write_text(automation.PREFERENCES, encoding="utf-8")
        log_dir = workdir / "logs"
        user_folder = workdir / "user"
        log_dir.mkdir()
        user_folder.mkdir()
        environment = dict(
            os.environ,
            SDL_VIDEO_DRIVER="dummy",
            SDL_AUDIO_DRIVER="dummy",
            SDL_RENDER_DRIVER="software",
        )
        err_path = workdir / "stderr.txt"
        err_file = err_path.open("w", encoding="utf-8")
        process = subprocess.Popen(
            [
                *RUNNER,
                str(args.native),
                "--game-dir",
                str(args.game_dir),
                "--skip-intro",
                "--mute",
                "--mcp-http",
                "--preferences-file",
                str(preferences),
                "--log-dir",
                str(log_dir),
                "--user-folder",
                str(user_folder),
            ],
            cwd=workdir,
            env=environment,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=err_file,
        )
        try:
            endpoint = wait_endpoint(workdir / "mcp.json", START_TIMEOUT)
            if os.name == "posix":
                mode = (workdir / "mcp.json").stat().st_mode & 0o777
                require(mode == stat.S_IRUSR | stat.S_IWUSR, f"mcp.json mode is {mode:o}")
            parsed = urlparse(endpoint["address"])
            require(parsed.hostname in ("127.0.0.1", "::1"), "the endpoint is not loopback")
            require(parsed.path == "/mcp", "the endpoint path is not /mcp")
            require(isinstance(endpoint["pid"], int), "the endpoint has no process id")
            host = parsed.hostname
            port = parsed.port
            token = endpoint["token"]
            status, session_id, started = post(
                host,
                port,
                token,
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "protocolVersion": "2025-06-18",
                        "capabilities": {},
                        "clientInfo": {"name": "native-mcp-http", "version": "0"},
                    },
                },
            )
            require(status == 200, f"initialize answered HTTP {status}")
            require(session_id, "initialize did not name a session")
            require(started, "initialize had no message")
            result = started[0].get("result") or {}
            require(
                result.get("protocolVersion") == "2025-06-18",
                f"protocol is {result.get('protocolVersion')}",
            )
            noted, _, _ = post(
                host,
                port,
                token,
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                session_id,
            )
            require(noted == 202, f"initialized answered HTTP {noted}")
            status_code, _, status_messages = post(
                host,
                port,
                token,
                {
                    "jsonrpc": "2.0",
                    "id": 2,
                    "method": "tools/call",
                    "params": {"name": "game.status"},
                },
                session_id,
            )
            require(status_code == 200, f"game.status answered HTTP {status_code}")
            require(status_messages, "game.status had no message")
            content = ((status_messages[0].get("result") or {}).get("content") or [{}])[0]
            body = json.loads(content.get("text") or "")
            require("tick" in body and body.get("in_match") is False, f"status is {body}")
            quit_code, _, quit_messages = post(
                host,
                port,
                token,
                {
                    "jsonrpc": "2.0",
                    "id": 3,
                    "method": "tools/call",
                    "params": {"name": "game.quit"},
                },
                session_id,
            )
            require(quit_code == 200, f"game.quit answered HTTP {quit_code}")
            require(quit_messages, "game.quit had no message")
            quit_text = ((quit_messages[0].get("result") or {}).get("content") or [{}])[0].get(
                "text"
            )
            require(json.loads(quit_text or "") == {"quitting": True}, "game.quit did not answer")
            code = process.wait(timeout=END_TIMEOUT)
            err_file.close()
            err = err_path.read_text(encoding="utf-8", errors="replace")
            require(code == 0, f"the game exited {code}\n{err[-2000:]}")
        finally:
            err_file.close()
            if process.poll() is None:
                process.kill()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
    print("native-mcp-http: ok")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (Failure, subprocess.TimeoutExpired, OSError, http.client.HTTPException) as error:
        print(f"native-mcp-http: {error}", file=sys.stderr)
        sys.exit(1)
