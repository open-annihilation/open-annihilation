#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check --mutator on a headless skirmish.

The game plays two commanders. With no mutator their health is neither 4000
nor 1234 nor 2000. ridge-tough sets both to 4000, and ridge-tough:health=2000
sets both to 2000, the number the option writes (1234 is not on that
option's step). ridge-tough then ridge-tougher leaves 5000, and the reverse
order leaves 4000. A missing id, an option the package does not have, a
mutator listed twice, a prerequisite that is not above it and two mutators
that conflict end with status 2, one refusal line and no match.
ridge-commander-boom ends with a non-zero status, a line naming the mutator
and its error, and no match. ridge-warp, whose hack is refused, above
ridge-commander-boom also ends with no match, and the start failure names
ridge-commander-boom, not ridge-warp. --no-mutators plays the stock health and
logs the refusal once. A match with mutators leaves no crash record and
writes no remembered set. A record planted beside the preferences is
logged once and deleted. A campaign start that the mutators refuse names
ridge-commander-boom and leaves no crash record. ridge-commander-copy, whose
manifest names no clash mode, clones CORCOM onto ARMCOM: with
mutators.clash-default=add in the preferences the copy takes a private name,
one note names the rename and both commanders keep their stock health; without
the key the copy replaces ARMCOM, whose commander then holds 4321, and no
rename is noted. ridge-roll sets both commanders' health from its random
draw. With ridge-aside, whose hack is refused, the start runs again from
ridge-roll alone: the commanders' health matches a skirmish of ridge-roll
by itself. A network game hosted with both records ridge-roll alone, the
host's commander holds that same health, and strict playback of the
recording rebuilds the recorded unit definitions. A list that applied in
full is not run again.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

import check_native_automation as automation
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))
PREPARED = "Offline match world prepared"
START_FAILED = "Skirmish start failed: "
UNIT_LINE = re.compile(r"^tick=(\d+) slot=\d+ type=\d+ owner=(\d+) .* health=(-?\d+)")
RUN_TIMEOUT = 300
# The first line of a preferences file.
PREFERENCES_HEADER = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"'
)
# The preference that sets the clash mode a mutator's manifest leaves unset.
CLASH_DEFAULT = "mutators.clash-default"
# The health ridge-commander-copy gives its copy of CORCOM.
COPY_HEALTH = 4321
RENAMED_NOTE = "ridge-commander-copy renamed ARMCOM to ARMCOM_2"
REPLACED_NOTE = "ridge-commander-copy replaced ARMCOM from the game on "
# Canonical loadout text for ridge-tough 1.0. The crash record stores this
# text; the next start names the id and the version and does not resolve it.
TOUGH_LOADOUT = (
    '{"mutators":[{"clashes":"replace","folder":false,"id":"ridge-tough","options":{},'
    '"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",'
    '"version":"1.0"}],"oaloadout":1}'
)
# Logged when a start or a room runs the pipeline again from the applied list.
RESEED = "mutators: seeded again from the mutators that applied, because "
# ridge-roll draws this into both commanders' health. ridge-aside's hack is
# refused, so it fails alone and the seed is ridge-roll's.
ROLL_ID = "ridge-roll"
ASIDE_ID = "ridge-aside"
ROLL_SHA = "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"
ASIDE_SHA = "b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90a1"
MUTATORS_SECTOR = 0x4F414D31
# Playback's unit table when every recorded definition is here and no other.
SAME_UNIT_TABLE = re.compile(
    r"^demo unit table: (\d+) recorded, \1 matched, 0 missing, 0 extra here$"
)
# Seconds a hosted game may take to reach a line, or to end.
ROOM_TIMEOUT = 120
LOOPBACK = "127.0.0.1"
ROLL_LUA = """-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return {
    units = function(units, ctx)
      units:where { commander = true }:set { maxdamage = 1000 + math.random(1, 5000) }
    end
  }
end
"""
# min 15 and max 5 break the profile's min <= max rule, so the hack is refused
# and the mutator fails alone.
ASIDE_LUA = """-- SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
-- SPDX-License-Identifier: GPL-3.0-only

function mutate(ctx)
  return { hacks = { ["console.game-speed-range"] = { min = 15, max = 5 } } }
end
"""


class ScriptFailure(Exception):
    """A behaviour that differs from the expected one."""


def run(native, game_dir, workdir, name, mutators, extra=()):
    """Runs one headless start. Returns the status and the output lines."""
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    digest = workdir / f"{name}.trace"
    units = workdir / f"{name}.units"
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--match-ticks",
        "2",
        "--seed",
        "1",
        "--trace-digest",
        str(digest),
        "--trace-units",
        str(units),
        "--preferences-file",
        str(workdir / f"{name}.conf"),
    ]
    command.extend(extra)
    for value in mutators:
        command += ["--mutator", value]
    result = subprocess.run(
        command,
        cwd=workdir,
        env=environment,
        timeout=RUN_TIMEOUT,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    output = result.stdout + result.stderr
    return result.returncode, output.splitlines(), units


def commanders(units_path, lines):
    """The two commanders' health at the earliest traced tick."""
    if not units_path.is_file():
        raise ScriptFailure("the units trace was not written")
    text = units_path.read_text(encoding="utf-8", errors="replace")
    found = []
    earliest = None
    for line in text.splitlines():
        match = UNIT_LINE.match(line)
        if match is None:
            continue
        tick = int(match.group(1))
        if earliest is None or tick < earliest:
            earliest = tick
            found = [(int(match.group(2)), int(match.group(3)))]
        elif tick == earliest:
            found.append((int(match.group(2)), int(match.group(3))))
    if len(found) != 2:
        print("\n".join(lines))
        raise ScriptFailure(f"the first tick has {len(found)} units, not two commanders")
    owners = {owner for owner, _health in found}
    if len(owners) != 2:
        raise ScriptFailure("the two commanders do not belong to two players")
    health = sorted(health for _owner, health in found)
    return health[0], health[1]


def host_commander(units_path, lines):
    """The host's commander's health at the earliest tick it is traced.

    The host is player 0. The other player's commander reaches the host some
    ticks later, as a remote unit whose health the host does not keep.
    """
    if not units_path.is_file():
        raise ScriptFailure("the units trace was not written")
    text = units_path.read_text(encoding="utf-8", errors="replace")
    found = []
    earliest = None
    for line in text.splitlines():
        match = UNIT_LINE.match(line)
        if match is None or int(match.group(2)) != 0:
            continue
        tick = int(match.group(1))
        if earliest is None or tick < earliest:
            earliest = tick
            found = [int(match.group(3))]
        elif tick == earliest:
            found.append(int(match.group(3)))
    if len(found) != 1:
        print("\n".join(lines))
        raise ScriptFailure(
            f"the host has {len(found)} units at its first tick, not its commander"
        )
    return found[0]


def expect_health(native, game_dir, workdir, name, mutators, health):
    """A skirmish whose two commanders hold one health."""
    status, lines, units = run(native, game_dir, workdir, name, mutators)
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"{name} exited with {status}")
    if not any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{name} prepared no match")
    if any(RESEED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{name} seeded again when every mutator applied")
    first, second = commanders(units, lines)
    if first != health or second != health:
        raise ScriptFailure(f"{name} left the commanders at {first} and {second}, not {health}")


def expect_stock(native, game_dir, workdir):
    """A skirmish with no mutator, at the commanders' own health."""
    status, lines, units = run(native, game_dir, workdir, "stock", [])
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"the stock skirmish exited with {status}")
    if not any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("the stock skirmish prepared no match")
    if any("mutators:" in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("a skirmish with no mutator logged mutators")
    first, second = commanders(units, lines)
    for health in (first, second):
        if health in (4000, 1234, 2000):
            raise ScriptFailure(f"the stock commanders hold {health}")
    return first, second


def separate(workdir, name):
    """A folder of one run, so its crash record is not another run's."""
    folder = workdir / name
    folder.mkdir()
    return folder


def assert_no_sentinel(folder):
    """The crash record is not left beside this run's preferences."""
    sentinel = folder / "mutators-sentinel.conf"
    if sentinel.exists():
        raise ScriptFailure(f"{sentinel} was left")


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def expect_no_mutators(native, game_dir, workdir, package, health):
    """--no-mutators plays the stock health and leaves no crash record."""
    folder = separate(workdir, "no-mutators")
    status, lines, units = run(
        native, game_dir, folder, "run", [package], extra=("--no-mutators",)
    )
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"--no-mutators exited with {status}")
    if not any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("--no-mutators prepared no match")
    first, second = commanders(units, lines)
    if (first, second) != health:
        raise ScriptFailure(
            f"--no-mutators left the commanders at {first} and {second}, not {health[0]} and {health[1]}"
        )
    said = [line for line in lines if "--no-mutators refused" in line]
    if len(said) != 1:
        print("\n".join(lines))
        raise ScriptFailure(f"--no-mutators reported the refusal {len(said)} times, not once")
    assert_no_sentinel(folder)


def expect_no_remembered_set(folder, name):
    """A match with mutators left no crash record and wrote no remembered set."""
    assert_no_sentinel(folder)
    preferences = folder / f"{name}.conf"
    if not preferences.is_file():
        return
    text = preferences.read_text(encoding="utf-8", errors="replace")
    if "open-annihilation.mutators." in text:
        raise ScriptFailure(f"{preferences.name} holds a remembered set")


def expect_planted_record(native, game_dir, workdir):
    """A record planted before a stock run is logged once and deleted."""
    folder = separate(workdir, "planted")
    sentinel = folder / "mutators-sentinel.conf"
    sentinel.write_text(
        "open-annihilation-preferences 1\n"
        + quote_preference("mutators")
        + " "
        + quote_preference(TOUGH_LOADOUT)
        + "\n"
        + quote_preference("setup")
        + " "
        + quote_preference("none")
        + "\n",
        encoding="utf-8",
    )
    status, lines, _units = run(native, game_dir, folder, "run", [])
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"the planted record's run exited with {status}")
    phrase = "the last game ended in a crash with ridge-tough 1.0"
    said = [line for line in lines if phrase in line]
    if len(said) != 1:
        print("\n".join(lines))
        raise ScriptFailure(f"the planted record was reported {len(said)} times, not once")
    assert_no_sentinel(folder)


def expect_clash_default(native, game_dir, workdir, package, stock):
    """The preference sets the clash mode of a mutator whose manifest names none.

    With mutators.clash-default=add the clone onto ARMCOM takes a private
    name and both commanders keep stock; with no key it replaces ARMCOM.
    """
    for mode in ("add", None):
        name = "clash-" + (mode or "unset")
        folder = separate(workdir, name)
        if mode is not None:
            (folder / "run.conf").write_text(
                PREFERENCES_HEADER
                + "\n"
                + quote_preference(CLASH_DEFAULT)
                + " "
                + quote_preference(mode)
                + "\n",
                encoding="utf-8",
            )
        status, lines, units = run(native, game_dir, folder, "run", [str(package)])
        if status != 0:
            print("\n".join(lines))
            raise ScriptFailure(f"{name} exited with {status}")
        renamed = [line for line in lines if RENAMED_NOTE in line]
        replaced = [line for line in lines if REPLACED_NOTE in line]
        health = commanders(units, lines)
        if mode == "add":
            if len(renamed) != 1 or replaced:
                print("\n".join(lines))
                raise ScriptFailure(f"{name} did not keep the copy beside ARMCOM")
            if health != stock:
                raise ScriptFailure(f"{name} left the commanders at {health}, not {stock}")
        else:
            if renamed or len(replaced) != 1:
                print("\n".join(lines))
                raise ScriptFailure(f"{name} did not put the copy in place of ARMCOM")
            if COPY_HEALTH not in health:
                raise ScriptFailure(
                    f"{name} left the commanders at {health}, neither at {COPY_HEALTH}"
                )
        assert_no_sentinel(folder)


def expect_campaign_refusal(native, game_dir, workdir, package):
    """A campaign start the mutators refuse names that mutator and leaves no record."""
    mutator_id = "ridge-commander-boom"
    folder = separate(workdir, "campaign-boom")
    status, lines, _units = run(
        native,
        game_dir,
        folder,
        "run",
        [str(package / mutator_id)],
        extra=("--campaign", "Arm Campaign", "--mission", "1"),
    )
    if status == 0:
        print("\n".join(lines))
        raise ScriptFailure("the campaign refusal exited with status 0")
    named = False
    for line in lines:
        at = line.find(mutator_id)
        if at >= 0 and "boom" in line[at + len(mutator_id):]:
            named = True
    if not named:
        print("\n".join(lines))
        raise ScriptFailure(f"the campaign refusal did not name {mutator_id}")
    assert_no_sentinel(folder)


def expect_refused(native, game_dir, workdir, name, mutators, phrase, status_code):
    """A start that is refused and prepares no match."""
    status, lines, _units = run(native, game_dir, workdir, name, mutators)
    if status != status_code:
        print("\n".join(lines))
        raise ScriptFailure(f"{name} exited with {status}, not {status_code}")
    if any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{name} prepared a match")
    said = [line for line in lines if phrase in line]
    if len(said) != 1:
        print("\n".join(lines))
        raise ScriptFailure(f"{name} reported the refusal {len(said)} times, not once")


def expect_boom(native, game_dir, workdir, package):
    """A mutator whose script stops after a write, with no match prepared."""
    mutator_id = "ridge-commander-boom"
    status, lines, _units = run(native, game_dir, workdir, "boom", [str(package / mutator_id)])
    if status == 0:
        print("\n".join(lines))
        raise ScriptFailure(f"{mutator_id} exited with status 0")
    if any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure(f"{mutator_id} prepared a match")
    named = False
    for line in lines:
        at = line.find(mutator_id)
        if at >= 0 and "boom" in line[at + len(mutator_id):]:
            named = True
    if not named:
        print("\n".join(lines))
        raise ScriptFailure(f"{mutator_id} was not named with its error")


def package_text(package_id, layer):
    """A one-layer manifest."""
    return (
        "oamut: 1\n"
        f"id: {package_id}\n"
        f'name: "{package_id}"\n'
        'version: "1.0"\n'
        "author:\n"
        '  name: "Ridge Maker"\n'
        'tags: ["gameplay"]\n'
        'works-with: ["*"]\n'
        f'layers: ["{layer}"]\n'
    )


def install_seed_packages(user):
    """Installs ridge-roll and ridge-aside, each with an origin record."""
    specs = (
        (ROLL_ID, "units", ROLL_LUA, ROLL_SHA),
        (ASIDE_ID, "hacks", ASIDE_LUA, ASIDE_SHA),
    )
    for package_id, layer, lua, sha in specs:
        folder = user / "Mutators" / package_id
        folder.mkdir(parents=True)
        (folder / "oamut.yaml").write_text(package_text(package_id, layer), encoding="utf-8")
        (folder / "mutator.lua").write_text(lua, encoding="utf-8")
        (folder / ".oa-origin.yaml").write_text(
            "oa-origin: 1\norigin: file\n"
            f'sha256: "{sha}"\n'
            'installed: "2026-10-22"\n',
            encoding="utf-8",
        )


def reseed_lines(lines):
    """The lines that say the pipeline ran again."""
    return [line for line in lines if RESEED in line]


def expect_seeded_skirmish(native, game_dir, workdir, user):
    """A failing companion leaves the health of the mutator that applied.

    The start logs one reseed line naming the companion. A skirmish of the
    applied mutator alone logs none, and both commanders hold the same health.
    """
    folder = separate(workdir, "with-aside")
    status, lines, units = run(
        native, game_dir, folder, "run", [ROLL_ID, ASIDE_ID],
        extra=("--user-folder", str(user)),
    )
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"the companion skirmish exited with {status}")
    seeded = reseed_lines(lines)
    if len(seeded) != 1 or ASIDE_ID not in seeded[0] or ROLL_ID in seeded[0]:
        print("\n".join(lines))
        raise ScriptFailure(f"the companion skirmish seeded again {len(seeded)} times")
    playing = [line for line in lines if "mutators in play:" in line]
    if len(playing) != 1 or ROLL_ID not in playing[0] or ASIDE_ID in playing[0]:
        print("\n".join(lines))
        raise ScriptFailure("the companion skirmish did not play ridge-roll alone")
    health = commanders(units, lines)
    if health[0] != health[1]:
        raise ScriptFailure(f"the companion skirmish left the commanders at {health}")
    alone = separate(workdir, "roll-alone")
    status, lines, units = run(
        native, game_dir, alone, "run", [ROLL_ID], extra=("--user-folder", str(user)),
    )
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"ridge-roll alone exited with {status}")
    if reseed_lines(lines):
        print("\n".join(lines))
        raise ScriptFailure("ridge-roll alone seeded again")
    if commanders(units, lines) != health:
        raise ScriptFailure(
            f"ridge-roll alone left {commanders(units, lines)}, not {health}"
        )
    return health


def free_port():
    """A UDP port of 127.0.0.1 that nothing holds now."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind((LOOPBACK, 0))
        return probe.getsockname()[1]


class HostedGame:
    """One game in the main loop, whose output the check reads as it comes."""

    def __init__(self, native, game_dir, workdir, name, user, arguments, fark_file):
        self.name = name
        self.lines = []
        self.fark_file = fark_file
        self._queue = queue.Queue()
        self._ended = False
        environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy",
                           SDL_RENDER_DRIVER="software")
        fark_file.parent.mkdir(parents=True, exist_ok=True)
        preferences = workdir / f"{name}.conf"
        if not preferences.exists():
            # No preferences means the defaults, which leave both online checks on.
            preferences.write_text(PREFERENCES_HEADER + "\n", encoding="utf-8")
        data_dir = workdir / f"{name}.data"
        data_dir.mkdir(exist_ok=True)
        command = [
            *RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
            "--preferences-file", str(preferences), "--user-folder", str(user),
            "--data-dir", str(data_dir), "--fark", "--fark-file", str(fark_file), *arguments,
        ]
        self.process = subprocess.Popen(
            command, cwd=workdir, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, errors="replace",
        )
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.process.stdout:
            self._queue.put(line.rstrip("\n"))
        self._queue.put(None)

    def _take(self, timeout):
        if self._ended:
            return False
        try:
            line = self._queue.get(timeout=timeout)
        except queue.Empty:
            return True
        if line is None:
            self._ended = True
            return False
        self.lines.append(line)
        return True

    def drain(self):
        while True:
            if self._ended:
                return
            try:
                line = self._queue.get_nowait()
            except queue.Empty:
                return
            if line is None:
                self._ended = True
                return
            self.lines.append(line)

    def wait_for(self, expected):
        deadline = time.monotonic() + ROOM_TIMEOUT
        while expected not in self.lines and time.monotonic() < deadline:
            if not self._take(0.5):
                break
        if expected not in self.lines:
            raise ScriptFailure(f"{self.name} never printed: {expected}")

    def wait_end(self):
        try:
            status = self.process.wait(timeout=ROOM_TIMEOUT)
        except subprocess.TimeoutExpired:
            self.stop()
            raise ScriptFailure(f"{self.name} did not end within {ROOM_TIMEOUT} s") from None
        # The reader can still be copying the last lines after the process ends.
        deadline = time.monotonic() + 5
        while self._take(0.2) and time.monotonic() < deadline:
            pass
        if status != 0:
            raise ScriptFailure(f"{self.name} exited with {status}")

    def stop(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()

    def output(self):
        self.drain()
        return "\n".join(f"{self.name}: {line}" for line in self.lines)


def open_client(game):
    """Connects to the game's automation endpoint and says hello."""
    deadline = time.monotonic() + ROOM_TIMEOUT
    while time.monotonic() < deadline:
        game.drain()
        if game.fark_file.is_file():
            break
        if game.process.poll() is not None:
            raise ScriptFailure(f"{game.name} ended before it served automation")
        time.sleep(0.1)
    else:
        raise ScriptFailure(f"{game.name} wrote no automation file")
    endpoint = json.loads(game.fark_file.read_text(encoding="utf-8"))
    client = automation.Client(endpoint["address"])
    hello = client.request("hello", versions=[1], client="seeded-replay", token=endpoint["token"])
    if not hello.get("ok"):
        raise ScriptFailure(f"{game.name} answered hello with {hello}")
    return client


def click_ready(client):
    """Clicks this machine's READY and waits until its player is ready."""
    room = client.request("room")
    name = f"READY{room.get('local_slot')}"
    slot = room.get("local_slot")
    deadline = time.monotonic() + ROOM_TIMEOUT
    clicked = False
    while time.monotonic() < deadline:
        shown = client.request("screen")
        if shown.get("screen") == "mp_battleroom" and not clicked:
            for control in client.request("controls").get("controls", []):
                if control.get("name", "").lower() == name.lower() and control.get("enabled"):
                    automation.click_control(client, name, "mp_battleroom")
                    clicked = True
                    break
        pictured = client.request("room")
        for player in pictured.get("players") or []:
            if player.get("slot") == slot and player.get("ready"):
                return
        time.sleep(0.1)
    raise ScriptFailure(f"the battle room never readied {name}")


def recording_sectors(path):
    """The recording's sectors as (type, data) pairs."""
    data = path.read_bytes()
    chunks = []
    pos = 0
    while pos + 2 <= len(data):
        length = int.from_bytes(data[pos:pos + 2], "little")
        if length < 2 or pos + length > len(data):
            break
        chunks.append(data[pos + 2:pos + length])
        pos += length
    if len(chunks) < 2 or len(chunks[1]) < 4:
        raise ScriptFailure(f"{path.name} has no sector count")
    count = int.from_bytes(chunks[1][:4], "little")
    bodies = chunks[2:2 + count]
    parsed = []
    for body in bodies:
        if len(body) < 4:
            raise ScriptFailure(f"{path.name} has a short sector")
        parsed.append((int.from_bytes(body[:4], "little"), body[4:]))
    return parsed


def expect_seeded_replay(native, game_dir, workdir):
    """A recording made with a failing companion replays the layer its match played."""
    user = workdir / "seed-user"
    install_seed_packages(user)
    health = expect_seeded_skirmish(native, game_dir, workdir, user)
    joiner_user = workdir / "seed-joiner-user"
    shutil.copytree(user, joiner_user)
    recorded = workdir / "seeded.tad"
    units = workdir / "seeded.units"
    port = free_port()
    games = []
    try:
        host = HostedGame(
            native, game_dir, workdir, "seed-host", user,
            ["--dplay-port", str(port), "--host", "--player-name", "Hoster",
             "--game-name", "Seeded Room", "--mutator", ROLL_ID, "--mutator", ASIDE_ID,
             "--net-record", str(recorded), "--trace-digest", str(workdir / "seeded.trace"),
             "--trace-units", str(units)],
            workdir / "seed-host-fark" / "automation.json",
        )
        games.append(host)
        host.wait_for('multiplayer: in the battle room of "Seeded Room" as Hoster')
        visitor = HostedGame(
            native, game_dir, workdir, "seed-joiner", joiner_user,
            ["--dplay-port", str(port), "--join", LOOPBACK, "--player-name", "Visitor"],
            workdir / "seed-joiner-fark" / "automation.json",
        )
        games.append(visitor)
        visitor.wait_for('multiplayer: in the battle room of "Seeded Room" as Visitor')
        host.wait_for("multiplayer: mutators allow START")
        host_client = open_client(host)
        guest_client = open_client(visitor)
        try:
            automation.wait_screen(guest_client, "mp_battleroom")
            automation.wait_screen(host_client, "mp_battleroom")
            click_ready(guest_client)
            click_ready(host_client)
            deadline = time.monotonic() + ROOM_TIMEOUT
            while time.monotonic() < deadline:
                shown = host_client.request("screen")
                if shown.get("screen") == "mp_battleroom":
                    start = None
                    for control in host_client.request("controls").get("controls", []):
                        if control.get("name", "").lower() == "start" and control.get("enabled"):
                            start = control
                            break
                    if start is not None:
                        automation.click_control(host_client, "START", "mp_battleroom")
                        break
                time.sleep(0.1)
            else:
                raise ScriptFailure("START stayed unavailable")
            # The recording is written when the host leaves, so the line that
            # names the file comes after quit. The units trace is flushed each
            # tick once the match is running.
            deadline = time.monotonic() + ROOM_TIMEOUT
            while time.monotonic() < deadline:
                host.drain()
                visitor.drain()
                screen = host_client.request("screen")
                if screen.get("screen") == "main_menu":
                    raise ScriptFailure("the host returned to the menu before a tick")
                if (screen.get("tick") or 0) >= 1 and units.is_file():
                    break
                time.sleep(0.2)
            else:
                raise ScriptFailure("the recorded match did not reach a tick")
            host_client.request("quit")
            guest_client.request("quit")
        finally:
            host_client.close()
            guest_client.close()
        host.wait_end()
        visitor.wait_end()
        if not any("multiplayer: recorded the game to " in line for line in host.lines):
            print(host.output())
            raise ScriptFailure("the host did not record the game")
    except ScriptFailure:
        for game in games:
            print(game.output())
            game.stop()
        raise
    except Exception:
        for game in games:
            game.stop()
        raise
    match_health = host_commander(units, host.lines)
    if match_health != health[0]:
        raise ScriptFailure(
            f"the recorded match left the host's commander at {match_health}, "
            f"not the skirmish's {health[0]}"
        )
    sectors = recording_sectors(recorded)
    loadouts = [body for kind, body in sectors if kind == MUTATORS_SECTOR]
    if len(loadouts) != 1:
        raise ScriptFailure(f"the recording holds {len(loadouts)} mutator sectors, not one")
    text = loadouts[0].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    if f'"id":"{ROLL_ID}"' not in text or ASIDE_ID in text:
        raise ScriptFailure(f"the recording kept {text}")
    played = separate(workdir, "replay")
    status, lines, _units = run(
        native, game_dir, played, "run", [],
        extra=("--user-folder", str(user), "--play-demo", str(recorded),
               "--demo-unit-table", "strict"),
    )
    if status != 0:
        print("\n".join(lines))
        raise ScriptFailure(f"the replay exited with {status}")
    if reseed_lines(lines):
        print("\n".join(lines))
        raise ScriptFailure("the replay seeded again")
    if not any(line == "demo mutators: 1 applied" for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("the replay did not apply one mutator")
    # Playback's units are remote, so its units trace shows no health. The
    # unit table keys each definition by its FBI bytes, so a layer that drew
    # another health would leave the commanders missing here.
    if not any(SAME_UNIT_TABLE.match(line) for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("the replay did not rebuild the recording's unit definitions")


def expect_stopper_named(native, game_dir, workdir, package):
    """A refused hack above a hook error: the start failure names the hook."""
    stopper = "ridge-commander-boom"
    mutators = [str(package / "ridge-warp"), str(package / stopper)]
    status, lines, _units = run(native, game_dir, workdir, "warp-boom", mutators)
    if status == 0:
        print("\n".join(lines))
        raise ScriptFailure("ridge-warp above ridge-commander-boom exited with status 0")
    if any(PREPARED in line for line in lines):
        print("\n".join(lines))
        raise ScriptFailure("ridge-warp above ridge-commander-boom prepared a match")
    failed = [line for line in lines if START_FAILED in line]
    if len(failed) != 1 or f"({stopper}): hooks: " not in failed[0] or "ridge-warp" in failed[0]:
        print("\n".join(lines))
        raise ScriptFailure(f"the start failure did not name {stopper} alone")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True, help="open-annihilation")
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--packages", type=Path, required=True, help="tests/data/mutators/packages")
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    packages = args.packages.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    tough = str(packages / "ridge-tough")
    tougher = str(packages / "ridge-tougher")
    with tempfile.TemporaryDirectory(prefix="native-mutators-", dir=args.scratch_root.resolve()) as scratch:
        workdir = Path(scratch)
        try:
            health = expect_stock(native, game_dir, workdir)
            expect_health(native, game_dir, workdir, "tough", [tough], 4000)
            expect_no_remembered_set(workdir, "tough")
            expect_health(native, game_dir, workdir, "option", [tough + ":health=2000"], 2000)
            expect_health(native, game_dir, workdir, "order", [tough, tougher], 5000)
            expect_health(native, game_dir, workdir, "reverse", [tougher, tough], 4000)
            expect_refused(
                native,
                game_dir,
                workdir,
                "missing",
                ["ridge-nothere"],
                "is not an installed mutator",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "speed",
                [tough + ":speed=2"],
                "has no option speed",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "twice",
                [tough, tough],
                "is listed twice",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "needs",
                [str(packages / "ridge-needs-swift")],
                "needs ridge-swift >= 1.0 above it",
                2,
            )
            expect_refused(
                native,
                game_dir,
                workdir,
                "conflict",
                [str(packages / "ridge-swift"), str(packages / "ridge-against-swift")],
                "conflicts with ridge-swift",
                2,
            )
            expect_boom(native, game_dir, workdir, packages)
            expect_stopper_named(native, game_dir, workdir, packages)
            expect_no_mutators(native, game_dir, workdir, tough, health)
            expect_planted_record(native, game_dir, workdir)
            expect_campaign_refusal(native, game_dir, workdir, packages)
            expect_clash_default(
                native, game_dir, workdir, packages / "ridge-commander-copy", health
            )
            expect_seeded_replay(native, game_dir, workdir)
        except ScriptFailure as failure:
            print(f"FAIL {failure}")
            return 1
    print("mutators command line check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
