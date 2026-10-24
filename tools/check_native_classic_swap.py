#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Play a pinned computer-player check twice, the second time as Classic.

The second run writes open-annihilation.brain.default as classic and installs
brains/classic under Brains/classic in the user folder beside the preferences
file. The skirmish and the loopback computers take that default. A Classic
run must name classic 1.0 for every computer seat and must not stop.

trace compares --trace-digest streams and --trace-units dumps at 30 and
3,000 ticks. stage compares the saveload digest line at 90 and 3,000 ticks,
and at 90 ticks also the stage check's placements, counts and orders.
director requires the pinned frame and sound hashes. The loopback checks
use the same commands as the built-in computer checks.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through. Empty runs the executable.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

PREFERENCES_HEADER = "open-annihilation-preferences 1"
BRAIN_KEY = "open-annihilation.brain.default"
USER_FOLDER_NAME = "Open Annihilation"
TRACE_SEED = 1234567
TRACE_TICKS = (30, 3000)
STAGE_SEED = 4242
STAGE_TICKS = (90, 3000)
LOOPBACK_TICKS = 6000
# One game stays inside the native-check budget when the script runs two.
RUN_TIMEOUT_SECONDS = 840

# The pin native-director-render requires of the headless skirmish.
DIRECTOR_HASH = (
    "director render: frames ec9f8e0c7906ef500077571b2da8baf5a8ad6904ca7d3aa44843b13fce496372 "
    "sound 148e026257cb31ce5afc3f1665856ef483a3983ad74a1fcb50be78d82ec3992b"
)
DIRECTOR_FAIL = "director render check:"
# Replaced by --director-hash so the ctest pin stays the one source.

STAGE = """# The local player's base and the computer player's raid.
unit 0 ARMSOLAR -150 -100
unit 0 ARMSOLAR -150 50
unit 0 ARMAMD 150 0
stockpile 2
unit 1 CORAK 0 250 0
unit 1 CORAK 40 250 0
group builder
unit 0 ARMCK 0 -200
move-order builder 0
build ARMSOLAR 0 -100
"""

PLAYS = re.compile(r"brains: player (\d+) plays classic 1\.0\b")
STOP_NOTICE = "the engine plays this seat on"
PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player (\d+) at (-?\d+),(-?\d+)$", re.M)
TYPES = re.compile(r"^saveload: unit types \d+((?: \S+=\d+)*)$", re.M)
ORDERS = re.compile(r"^saveload: orders \d+((?: \S+=\d+)*)$", re.M)
DIGEST = re.compile(r"^saveload: tick (\d+) units (\d+) digest ([0-9a-f]{16})$", re.M)

TRACE_MAGIC = 0x3154414F
TRACE_VERSION = 1
TRACE_HEADER_SIZE = 16
TRACE_RECORD_SIZE = 24
TRACE_KIND_TICK = 1
TRACE_KIND_SECTION = 0x10
TRACE_SECTIONS = 7


class SwapFailure(Exception):
    """A Classic run that is not the built-in player's game."""


def quote_preference(text):
    """One preferences token, quoted as the file stores it."""
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_preferences(path, classic):
    """A preferences file. Classic runs name classic as the default brain."""
    lines = [PREFERENCES_HEADER]
    if classic:
        lines.append(quote_preference(BRAIN_KEY) + " " + quote_preference("classic"))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def install_classic(preferences, package):
    """Copies the Classic package into the user folder beside the preferences."""
    folder = preferences.parent / USER_FOLDER_NAME / "Brains" / "classic"
    if folder.exists():
        shutil.rmtree(folder)
    folder.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(package, folder)


def seat_log_dirs():
    """Folders that can hold logs/brains from the data directory."""
    home = Path.home()
    found = []
    support = home / "Library" / "Application Support"
    for older in ("net.coreprime.open-annihilation", "com.coreprime.open-annihilation"):
        found.append(support / older / "logs" / "brains")
    xdg = os.environ.get("XDG_DATA_HOME")
    if xdg:
        found.append(Path(xdg) / "open-annihilation" / "logs" / "brains")
    found.append(home / ".local" / "share" / "open-annihilation" / "logs" / "brains")
    local = os.environ.get("LOCALAPPDATA")
    if local:
        found.append(Path(local) / "CorePrime" / "Open Annihilation" / "logs" / "brains")
    return found


def snapshot_logs(folders):
    """Modification times of seat logs, so an older log is not this run."""
    seen = {}
    for folder in folders:
        if not folder.is_dir():
            continue
        for path in folder.glob("seat-*.log"):
            try:
                seen[path] = path.stat().st_mtime_ns
            except OSError:
                continue
    return seen


def changed_logs(before, folders, work):
    """Seat logs written or replaced since `before`, plus any under `work`."""
    changed = []
    for folder in folders:
        if not folder.is_dir():
            continue
        for path in folder.glob("seat-*.log"):
            try:
                stamped = path.stat().st_mtime_ns
            except OSError:
                continue
            if before.get(path) != stamped:
                changed.append(path)
    if work.is_dir():
        for path in work.rglob("seat-*.log"):
            changed.append(path)
    return changed


def log_stopped(paths):
    """True when a seat log records a stop."""
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if ": stopped:" in text:
            return path
    return None


def output_of(result):
    """The combined log of one run."""
    return result.stdout + result.stderr


def launch(native, game_dir, work, preferences, args, timeout):
    """Runs one game. The arguments are the pinned command, then preferences."""
    environment = dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
    )
    command = [
        *RUNNER,
        str(native),
        "--game-dir",
        str(game_dir),
        *args,
        "--preferences-file",
        str(preferences),
    ]
    return subprocess.run(
        command,
        cwd=work,
        env=environment,
        timeout=timeout,
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )


def run_once(native, game_dir, scratch, package, label, args, classic, timeout):
    """One built-in run, or one Classic run in its own folder."""
    work = Path(tempfile.mkdtemp(prefix=f"native-classic-{label}-", dir=scratch))
    preferences = work / "preferences.conf"
    write_preferences(preferences, classic)
    if classic:
        install_classic(preferences, package)
    before = snapshot_logs(seat_log_dirs())
    try:
        result = launch(native, game_dir, work, preferences, args, timeout)
    except subprocess.TimeoutExpired as expired:
        raise SwapFailure(f"{label}: the game timed out") from expired
    stopped = log_stopped(changed_logs(before, seat_log_dirs(), work))
    return result, stopped


def require_classic(result, stopped, label, computers):
    """The Classic run seated classic 1.0 and did not hand the seat back."""
    text = output_of(result)
    if result.returncode != 0:
        print(text, end="" if text.endswith("\n") or text == "" else "\n")
        raise SwapFailure(f"{label}: exited with {result.returncode}")
    if STOP_NOTICE in text:
        print(text, end="" if text.endswith("\n") or text == "" else "\n")
        raise SwapFailure(f"{label}: Classic stopped and the built-in player took the seat")
    if stopped is not None:
        raise SwapFailure(f"{label}: {stopped} records a stop")
    seated = PLAYS.findall(text)
    if len(seated) < computers:
        print(text, end="" if text.endswith("\n") or text == "" else "\n")
        raise SwapFailure(
            f"{label}: {len(seated)} classic 1.0 seat lines, {computers} computer seats"
        )
    return text


def require_builtin(result, label):
    """The built-in run finished, and no seat played Classic."""
    text = output_of(result)
    if result.returncode != 0:
        print(text, end="" if text.endswith("\n") or text == "" else "\n")
        raise SwapFailure(f"{label}: the built-in run exited with {result.returncode}")
    if PLAYS.search(text):
        raise SwapFailure(f"{label}: the built-in run seated Classic")
    return text


def pair(native, game_dir, scratch, package, label, args, computers, timeout=RUN_TIMEOUT_SECONDS):
    """The pinned command, then the same command with Classic."""
    builtin, _ = run_once(
        native, game_dir, scratch, package, label + "-builtin", args, False, timeout
    )
    classic, stopped = run_once(
        native, game_dir, scratch, package, label + "-classic", args, True, timeout
    )
    builtin_text = require_builtin(builtin, label)
    classic_text = require_classic(classic, stopped, label, computers)
    return builtin_text, classic_text


def read_trace(path):
    """The trace header seed and its records."""
    import struct

    raw = path.read_bytes()
    if len(raw) < TRACE_HEADER_SIZE:
        raise SwapFailure(f"{path}: no header")
    magic, version, record_size, seed = struct.unpack_from("<4I", raw)
    if (magic, version, record_size) != (TRACE_MAGIC, TRACE_VERSION, TRACE_RECORD_SIZE):
        raise SwapFailure(f"{path}: header {magic:#x}/{version}/{record_size}")
    if (len(raw) - TRACE_HEADER_SIZE) % TRACE_RECORD_SIZE:
        raise SwapFailure(f"{path}: a partial record at the end")
    body = raw[TRACE_HEADER_SIZE:]
    records = [
        struct.unpack_from("<6I", body, at) for at in range(0, len(body), TRACE_RECORD_SIZE)
    ]
    return seed, records


def check_trace_layout(path, ticks):
    """The trace check's layout: header, section order, both commanders."""
    seed, records = read_trace(path)
    if seed != TRACE_SEED:
        raise SwapFailure(f"{path}: header seed {seed}, not {TRACE_SEED}")
    per_tick = 1 + TRACE_SECTIONS
    if len(records) != ticks * per_tick:
        raise SwapFailure(f"{path}: {len(records)} records, not {ticks} ticks of {per_tick}")
    for tick in range(1, ticks + 1):
        record = records[(tick - 1) * per_tick]
        if record[:2] != (TRACE_KIND_TICK, tick):
            raise SwapFailure(f"{path}: tick {tick} does not open with its tick record")
        for section in range(TRACE_SECTIONS):
            record = records[(tick - 1) * per_tick + 1 + section]
            if record[:3] != (TRACE_KIND_SECTION, tick, section):
                raise SwapFailure(f"{path}: tick {tick} section {section} is out of place")
    if records[0][2] < 2:
        raise SwapFailure(f"{path}: {records[0][2]} live units at tick 1, not both commanders")


def compare_files(builtin_path, classic_path, label):
    """Byte-for-byte equality of two files the game wrote."""
    builtin_bytes = builtin_path.read_bytes()
    classic_bytes = classic_path.read_bytes()
    if not builtin_bytes or builtin_bytes != classic_bytes:
        raise SwapFailure(f"{label}: files differ ({builtin_path.name})")


def trace_args(work, ticks, stem):
    """The trace check's skirmish command, for one tick count."""
    return [
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--match-ticks",
        str(ticks),
        "--seed",
        str(TRACE_SEED),
        "--trace-digest",
        str(work / f"{stem}.trace"),
        "--trace-units",
        str(work / f"{stem}.units"),
    ]


def check_trace(native, game_dir, scratch, package):
    """30 and 3,000 ticks, streams and unit dumps byte-identical."""
    for ticks in TRACE_TICKS:
        label = f"trace-{ticks}"
        builtin_work = Path(tempfile.mkdtemp(prefix=f"native-classic-{label}-builtin-", dir=scratch))
        classic_work = Path(tempfile.mkdtemp(prefix=f"native-classic-{label}-classic-", dir=scratch))
        builtin_prefs = builtin_work / "preferences.conf"
        classic_prefs = classic_work / "preferences.conf"
        write_preferences(builtin_prefs, False)
        write_preferences(classic_prefs, True)
        install_classic(classic_prefs, package)
        builtin_args = trace_args(builtin_work, ticks, "builtin")
        classic_args = trace_args(classic_work, ticks, "classic")
        try:
            builtin = launch(
                native, game_dir, builtin_work, builtin_prefs, builtin_args, RUN_TIMEOUT_SECONDS
            )
            before = snapshot_logs(seat_log_dirs())
            classic = launch(
                native, game_dir, classic_work, classic_prefs, classic_args, RUN_TIMEOUT_SECONDS
            )
        except subprocess.TimeoutExpired as expired:
            raise SwapFailure(f"{label}: the game timed out") from expired
        require_builtin(builtin, label)
        stopped = log_stopped(changed_logs(before, seat_log_dirs(), classic_work))
        require_classic(classic, stopped, label, 1)
        for stem, folder in (("builtin", builtin_work), ("classic", classic_work)):
            check_trace_layout(folder / f"{stem}.trace", ticks)
        compare_files(builtin_work / "builtin.trace", classic_work / "classic.trace", label + " stream")
        compare_files(builtin_work / "builtin.units", classic_work / "classic.units", label + " units")
    print(
        f"native classic trace: {TRACE_TICKS[0]} and {TRACE_TICKS[1]} ticks, "
        "streams and unit dumps byte-identical, classic 1.0"
    )


def counts(line):
    """Name=count pairs from a saveload summary."""
    return {name: int(count) for name, count in re.findall(r"(\S+)=(\d+)", line)}


def require_stage_setup(output, label):
    """Placements, the stockpile ask and the standing move, as the stage check."""
    placed = PLACED.findall(output)
    if [(name, int(player)) for _, name, player, _, _ in placed] != [
        ("ARMSOLAR", 0),
        ("ARMSOLAR", 0),
        ("ARMAMD", 0),
        ("CORAK", 1),
        ("CORAK", 1),
        ("ARMCK", 0),
    ]:
        raise SwapFailure(f"{label}: the stage placed {placed}")
    if "stage: unit " + placed[2][0] + " stockpiles 2" not in output:
        raise SwapFailure(f"{label}: the stage did not ask the defence for its rounds")
    if "stage: move-order builder: 1 units to 0" not in output:
        raise SwapFailure(f"{label}: the stage did not set the kbot's standing move order")


def require_stage_end(output, label):
    """The 90-tick live units and saved orders the stage check requires."""
    types, orders = TYPES.search(output), ORDERS.search(output)
    if not types or not orders:
        raise SwapFailure(f"{label}: the run reported no units or orders")
    live = counts(types.group(1))
    if live.get("ARMSOLAR") != 3 or live.get("ARMAMD") != 1 or live.get("CORAK") != 2:
        raise SwapFailure(f"{label}: the live units are {live}")
    saved = counts(orders.group(1))
    if saved.get("BuildWeapon") != 1:
        raise SwapFailure(f"{label}: the saved orders are {orders.group(1)}, without the defence's rounds")
    if not re.search(r"^stage: unit \d+ builds ARMSOLAR at -?\d+,-?\d+$", output, re.M) or (
        saved.get("MobileBuild", 0) + saved.get("HelpBuild", 0) < 1
    ):
        raise SwapFailure(f"{label}: the saved orders are {orders.group(1)}, without the builder's")


def digest_line(output, ticks, label):
    """The whole `saveload: tick … digest` line, for this tick count."""
    found = DIGEST.search(output)
    if not found:
        raise SwapFailure(f"{label}: the run reported no saveload digest")
    if int(found.group(1)) != ticks:
        raise SwapFailure(f"{label}: the digest names tick {found.group(1)}, not {ticks}")
    return found.group(0)


def stage_args(work, ticks, stem):
    """The stage check's command, for one tick count."""
    stage = work / f"{stem}.stage"
    stage.write_text(STAGE, encoding="utf-8")
    return [
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--seed",
        str(STAGE_SEED),
        "--match-ticks",
        str(ticks),
        "--save-after",
        str(ticks),
        "--save-file",
        str(work / f"{stem}.sav"),
        "--stage",
        str(stage),
    ]


def check_stage(native, game_dir, scratch, package):
    """90 ticks with the stage check's assertions, and 3,000 ticks' digest line."""
    for ticks in STAGE_TICKS:
        label = f"stage-{ticks}"
        builtin_work = Path(tempfile.mkdtemp(prefix=f"native-classic-{label}-builtin-", dir=scratch))
        classic_work = Path(tempfile.mkdtemp(prefix=f"native-classic-{label}-classic-", dir=scratch))
        builtin_prefs = builtin_work / "preferences.conf"
        classic_prefs = classic_work / "preferences.conf"
        write_preferences(builtin_prefs, False)
        write_preferences(classic_prefs, True)
        install_classic(classic_prefs, package)
        try:
            builtin = launch(
                native,
                game_dir,
                builtin_work,
                builtin_prefs,
                stage_args(builtin_work, ticks, "builtin"),
                RUN_TIMEOUT_SECONDS,
            )
            before = snapshot_logs(seat_log_dirs())
            classic = launch(
                native,
                game_dir,
                classic_work,
                classic_prefs,
                stage_args(classic_work, ticks, "classic"),
                RUN_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired as expired:
            raise SwapFailure(f"{label}: the game timed out") from expired
        builtin_text = require_builtin(builtin, label)
        stopped = log_stopped(changed_logs(before, seat_log_dirs(), classic_work))
        classic_text = require_classic(classic, stopped, label, 1)
        require_stage_setup(builtin_text, label + " built-in")
        require_stage_setup(classic_text, label + " classic")
        if ticks == STAGE_TICKS[0]:
            require_stage_end(builtin_text, label + " built-in")
            require_stage_end(classic_text, label + " classic")
        builtin_line = digest_line(builtin_text, ticks, label + " built-in")
        classic_line = digest_line(classic_text, ticks, label + " classic")
        if builtin_line != classic_line:
            raise SwapFailure(f"{label}: digest lines differ ({builtin_line} / {classic_line})")
    print(
        f"native classic stage: {STAGE_TICKS[0]} and {STAGE_TICKS[1]} ticks, "
        "the same saveload digest line, classic 1.0"
    )


def check_director(native, game_dir, scratch, package, expect):
    """The director render's command, then the same command with Classic."""
    args = ["--mute", "--check-director-render"]
    builtin_text, classic_text = pair(
        native, game_dir, scratch, package, "director", args, 1
    )
    print(classic_text, end="" if classic_text.endswith("\n") or classic_text == "" else "\n")
    for label, text in (("built-in", builtin_text), ("classic", classic_text)):
        if DIRECTOR_FAIL in text:
            raise SwapFailure(f"director {label}: {DIRECTOR_FAIL}")
        if expect not in text:
            raise SwapFailure(f"director {label}: pinned frames and sound did not match")
    print("native classic director: pinned frames and sound, classic 1.0")


def loopback_args(kind, record):
    """The loopback computer command for one of the three checks."""
    args = [
        "--skip-intro",
        "--mute",
        "--headless-check",
        "--net-loopback-check",
        str(LOOPBACK_TICKS),
    ]
    if kind == "each-ai":
        args.extend(["--net-loopback-each-ai", "--net-record", str(record)])
    else:
        args.append("--net-loopback-computer")
        if kind == "watcher":
            args.append("--net-loopback-watcher")
    return args


def check_loopback(native, game_dir, scratch, package, kind):
    """One loopback computer check, built-in then Classic."""
    computers = 2 if kind == "each-ai" else 1
    record = scratch / f"{kind}.tad"
    args = loopback_args(kind, record)
    builtin_text, classic_text = pair(
        native, game_dir, scratch, package, "loopback-" + kind, args, computers
    )
    print(classic_text, end="" if classic_text.endswith("\n") or classic_text == "" else "\n")
    if kind == "each-ai":
        if "recording played back" not in builtin_text:
            raise SwapFailure("loopback each-ai: the built-in run did not play the recording back")
        if "recording played back" not in classic_text:
            raise SwapFailure("loopback each-ai: the Classic run did not play the recording back")
    print(f"native classic loopback {kind}: passed, classic 1.0")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--brain", type=Path, required=True)
    parser.add_argument("--director-hash", default=DIRECTOR_HASH)
    parser.add_argument(
        "mode",
        choices=("trace", "stage", "director", "loopback-computer", "loopback-watcher", "loopback-each-ai"),
    )
    args = parser.parse_args()
    game_dir = args.game_dir.resolve()
    package = args.brain.resolve()
    if not package.is_dir():
        print(f"classic swap: no brain package at {package}", file=sys.stderr)
        return 1
    native = args.native.resolve()
    scratch = args.scratch_root.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    try:
        if args.mode == "trace":
            check_trace(native, game_dir, scratch, package)
        elif args.mode == "stage":
            check_stage(native, game_dir, scratch, package)
        elif args.mode == "director":
            check_director(native, game_dir, scratch, package, args.director_hash)
        else:
            kind = args.mode.removeprefix("loopback-")
            check_loopback(native, game_dir, scratch, package, kind)
    except SwapFailure as failure:
        print(f"classic swap: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
