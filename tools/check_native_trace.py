#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check the trace stream open-annihilation writes with --trace-digest.

Two headless skirmishes from the same --seed must write the same stream and
unit dump byte for byte: a header naming the seed, then for every tick from 1
the tick record and the section records in section order, with both
commanders counted from the first tick. A run from another seed must part
from them at the generator word of the first tick.
"""
import argparse
import os
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 30
SEED = 1234567
OTHER_SEED = 7654321
MAGIC = 0x3154414F
VERSION = 1
HEADER_SIZE = 16
RECORD_SIZE = 24
KIND_TICK = 1
KIND_SECTION = 0x10
SECTIONS = 7



def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    from pathlib import Path
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = target.read_text(encoding='utf-8') if target.is_file() else 'open-annihilation-preferences 1\n'
    if 'open-annihilation.update-check' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.update-check" "never"\n'
    if 'open-annihilation.content-updates' not in text:
        if not text.endswith('\n'):
            text += '\n'
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding='utf-8')


def run(native, game_dir, workdir, *extra):
    profile = workdir / "preferences.conf"
    _offline_preferences(profile)
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute", "--headless-check",
         "--preferences-file", str(profile), *map(str, extra)],
        cwd=workdir, timeout=600, check=False, capture_output=True, text=True,
    )
    return result


def read_stream(path):
    raw = path.read_bytes()
    if len(raw) < HEADER_SIZE:
        raise SystemExit(f"{path}: no header")
    magic, version, record_size, seed = struct.unpack_from("<4I", raw)
    if (magic, version, record_size) != (MAGIC, VERSION, RECORD_SIZE):
        raise SystemExit(f"{path}: header {magic:#x}/{version}/{record_size}")
    if (len(raw) - HEADER_SIZE) % RECORD_SIZE:
        raise SystemExit(f"{path}: a partial record at the end")
    body = raw[HEADER_SIZE:]
    return seed, [struct.unpack_from("<6I", body, at) for at in range(0, len(body), RECORD_SIZE)]


def skirmish(native, game_dir, workdir, name, seed):
    stream = workdir / f"{name}.trace"
    units = workdir / f"{name}.units"
    result = run(native, game_dir, workdir, "--match-ticks", TICKS, "--seed", seed,
                 "--trace-digest", stream, "--trace-units", units)
    if result.returncode != 0:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"skirmish {name} exited with {result.returncode}")
    return stream, units


def check_layout(path, seed):
    stream_seed, records = read_stream(path)
    if stream_seed != seed:
        raise SystemExit(f"{path}: header seed {stream_seed}, not {seed}")
    per_tick = 1 + SECTIONS
    if len(records) != TICKS * per_tick:
        raise SystemExit(f"{path}: {len(records)} records, not {TICKS} ticks of {per_tick}")
    for tick in range(1, TICKS + 1):
        record = records[(tick - 1) * per_tick]
        if record[:2] != (KIND_TICK, tick):
            raise SystemExit(f"{path}: tick {tick} does not open with its tick record")
        for section in range(SECTIONS):
            record = records[(tick - 1) * per_tick + 1 + section]
            if record[:3] != (KIND_SECTION, tick, section):
                raise SystemExit(f"{path}: tick {tick} section {section} is out of place")
    if records[0][2] < 2:
        raise SystemExit(f"{path}: {records[0][2]} live units at tick 1, not both commanders")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-trace-", dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        a_stream, a_units = skirmish(native, game_dir, workdir, "a", SEED)
        b_stream, b_units = skirmish(native, game_dir, workdir, "b", SEED)
        records = check_layout(a_stream, SEED)
        if a_stream.read_bytes() != b_stream.read_bytes():
            raise SystemExit("two runs from the same seed wrote different streams")
        if not a_units.read_text() or a_units.read_text() != b_units.read_text():
            raise SystemExit("two runs from the same seed wrote different unit dumps")
        c_stream, _ = skirmish(native, game_dir, workdir, "c", OTHER_SEED)
        _, other = read_stream(c_stream)
        if other[0][5] == records[0][5]:
            raise SystemExit("another seed left the first tick's generator word unchanged")
        print(f"native trace: {TICKS} ticks repeat from seed {SEED}, "
              f"{records[0][2]} live units at tick 1, generator {records[0][5]:#010x}")


if __name__ == "__main__":
    main()
