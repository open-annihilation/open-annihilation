#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Save a headless skirmish at a tick, load it back and compare world digests.

Run A fights COMBAT_UNITS units a side beside the commanders, with a factory
queue, a building, a patrol, a guard and a move given, for SAVE_TICK ticks and
saves at that tick, so its closing digest is the world the save captured. An
Atlas picks a unit up onto its link piece and is flying it off at the save,
and a transport ship holds units that started aboard. Two ticks before the
save a tree catches fire, one feature starts its die sequence, another its
reclamate sequence and a fourth is cleared away, so the save holds them while
they play. Run B starts from the save without stepping and must print the
same tick, unit count, digest, orders and saved features. Run C resumes the
save for RESUME_TICKS ticks to show the loaded match keeps simulating and
keeps those orders, the transports still carrying their units. The save is
named in Chinese (SAVE_NAME), so that it is written and loaded by a name
outside any 8-bit code page.

The digest covers each unit's record, economy, weapons, COB script state,
movement state and saved orders, the map's metal, placing-player and sight
words, the camera and the meteor state, and every run must report that the
save wrote and the load restored all script, movement, economy and order
state. The saved features are compared as the Features section writes them:
the counts of normal, 3D and animating records, the animating ones playing a
burn, die or reclamate sequence, and a digest of the section, leaving out the
features on plots a load hides under the map's edges, which do not come back.
The digest names each feature type by its name, since a load orders the
feature type table its own way.

Two campaign missions then check carried units across a save and a load: one
where an Atlas starts with a unit aboard, on a map whose schema places
feature types beyond the map's own, one where Bears and transport ships start
loaded. Each is saved at CARRIED_SAVE_TICK and loaded back, and must come back
with the same unit digest, which covers each unit's carrier, its piece (none
for a unit in the hold, which is not drawn) and its attacker, the same orders,
BeCarried among them, and the same saved features.

Saves load at the unit limit they were saved with, Summary "maxunits", which
every run prints with the match's setting and the run-wide limit. Run A's
save is edited three ways: without the field it loads at the run-wide limit;
at OTHER_UNIT_LIMIT it builds its world at that limit and holds the same
world, and the save the game then writes holds that limit and loads back
into the same world, which keeps simulating at it; at a limit no unit pool
can hold it is refused. Each campaign save, edited to
CAMPAIGN_SAVE_UNIT_LIMIT, still plays at its mission's limit and the same
world, and only the run-wide limit takes the save's. A skirmish started with
the Unit limit setting at SETTING_UNIT_LIMIT plays at it, and its save loads
at it into the same world in a run whose setting is the default, and keeps
simulating at it.

Run A is played again under a mod profile that turns every limit and hack on
at its baseline preset (written from the engine's registry, as
baseline_profile_text does): it must save the same world. Its save holds the
profile's ModProfile account, so a run without the profile refuses it and a
run under the profile loads it into the same world; run A's own save, which
has no such account, loads under the profile too.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import tempfile
import zlib
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

# Wall-time limit per game run; generous because a loaded machine stretches
# the headless checks without extra CPU work.
RUN_TIMEOUT_SECONDS = 900

SAVE_TICK = 150
# Run A's save: two hanzi, U+5B58 U+6863.
SAVE_NAME = "\u5b58\u6863.sav"
RESUME_TICKS = 30
COMBAT_UNITS = 6
DIGEST = re.compile(r"^saveload: tick (\d+) units (\d+) digest ([0-9a-f]{16})$", re.M)
FAILURES = re.compile(r"; (save|restore) failures (\d+)$", re.M)
ORDERS = re.compile(r"^saveload: orders (\d+)((?: \S+=\d+)*)$", re.M)
FEATURES = re.compile(
    r"^saveload: features normal (\d+) 3d (\d+) animating (\d+) burn (\d+) die (\d+) "
    r"reclaim (\d+) digest ([0-9a-f]{16})$", re.M)
# Missions the given orders hold at the save: the commander's building, the
# lab's queue, the patrol, the guard, the move, the Atlas flying its unit off
# and the carried units.
SAVED_MISSIONS = {"MobileBuild", "BuildingBuild", "Patrol", "Follow_Ground", "Move_Ground",
                  "VTOL_Unload", "BeCarried"}
# Missions that still run after the resumed ticks.
RESUMED_MISSIONS = {"BuildingBuild", "Patrol", "Follow_Ground", "VTOL_Unload", "BeCarried"}
# Units aboard the Atlas and the transport ship at the save and after the
# resumed ticks, at the least.
TRANSPORTED = 1 + 3
# Campaign missions whose units start aboard transports, by campaign name
# (as its file names it) and mission index, and what carries them.
CARRIED_MISSIONS = (
    ("Core Campaign", 22, "an Atlas"),
    ("Arm Campaign - Core Contingency ", 11, "Bears and transport ships"),
)
CARRIED_SAVE_TICK = 20
LIMITS = re.compile(r"^saveload: units per player (\d+) setting (\d+) run-wide (\d+)$", re.M)
# The limit a skirmish plays at while nothing else sets it.
DEFAULT_UNIT_LIMIT = 250
# A limit other than the default, which only an edited save reaches today.
# An edited save keeps its units' ids, and at this limit they still lie in
# their owners' ranges: player 0's from 1, player 1's from 251, which is
# between 200 + 1 and 2 x 200.
OTHER_UNIT_LIMIT = 200
# A campaign save's limit, which only the run-wide limit takes: the mission
# plays at its own.
CAMPAIGN_SAVE_UNIT_LIMIT = 500
# A limit whose unit pool would need more than 65535 slots.
UNBUILDABLE_UNIT_LIMIT = 7000
# The Unit limit setting of a skirmish that plays above the default.
SETTING_UNIT_LIMIT = 500
# A preferences file's first line, and the Unit limit setting's key.
PREFERENCES_HEADER = (
    "open-annihilation-preferences 1\n"
    '"open-annihilation.update-check" "never"\n'
    '"open-annihilation.content-updates" "never"'
)
UNIT_LIMIT_KEY = "open-annihilation.unit-limit"
REFUSED_LIMIT = "unit limit out of range"

# The saved-game bank: a header, one record per account, then the name pool.
BANK_MAGIC = b"HAPIBANK"
BANK_POOL_OFFSET = 0xC
BANK_FIRST_ACCOUNT = 0x10
BANK_POOL_PACKED = 0x18
ACCOUNT_HEADER_BYTES = 0x20
ACCOUNT_SIZE = 0x0
ACCOUNT_NAME = 0x4
ACCOUNT_COUNTS = 0x8  # integers, reals, texts and blobs
ACCOUNT_PACKED = 0x18
INT_ENTRY_BYTES = 8
REAL_ENTRY_BYTES = 12
TEXT_ENTRY_BYTES = 8
BLOB_ENTRY_BYTES = 16
BLOB_DATA_OFFSET = 8  # a file position in the unpacked record
SUMMARY_ACCOUNT = "summary"
MAX_UNITS_FIELD = "maxunits"
# A packed block: "SQSH", its type, whether it is scrambled, its sizes.
SQUASH_MAGIC = b"SQSH"
SQUASH_TYPE = 0x5
SQUASH_SCRAMBLED = 0x6
SQUASH_PACKED_SIZE = 0x7
SQUASH_UNPACKED_SIZE = 0xB
SQUASH_HEADER_BYTES = 0x13
SQUASH_LZ77 = 1
SQUASH_ZLIB = 2
LZ77_WINDOW = 4096
LZ77_FIRST_WRITE = 1
LZ77_MINIMUM_MATCH = 2


def lz77_decode(packed, size):
    """The bytes an LZ77 block unpacks to."""
    window = bytearray(LZ77_WINDOW)
    write = LZ77_FIRST_WRITE
    out = bytearray()
    cursor = 0
    while len(out) < size:
        flags = packed[cursor]
        cursor += 1
        for bit in range(8):
            if flags & (1 << bit) == 0:
                value = packed[cursor]
                cursor += 1
                out.append(value)
                window[write] = value
                write = (write + 1) % LZ77_WINDOW
                continue
            token = packed[cursor] | packed[cursor + 1] << 8
            cursor += 2
            offset = token >> 4
            if offset == 0:
                return bytes(out)
            for step in range((token & 0xF) + LZ77_MINIMUM_MATCH):
                value = window[(offset + step) % LZ77_WINDOW]
                out.append(value)
                window[write] = value
                write = (write + 1) % LZ77_WINDOW
    return bytes(out)


def unsquash(block):
    """The bytes a packed block of the bank holds."""
    if block[:4] != SQUASH_MAGIC or block[SQUASH_SCRAMBLED] != 0:
        raise SystemExit("the save holds a block this check cannot read")
    packed_size, = struct.unpack_from("<I", block, SQUASH_PACKED_SIZE)
    unpacked_size, = struct.unpack_from("<I", block, SQUASH_UNPACKED_SIZE)
    payload = block[SQUASH_HEADER_BYTES:SQUASH_HEADER_BYTES + packed_size]
    if block[SQUASH_TYPE] == SQUASH_LZ77:
        return lz77_decode(payload, unpacked_size)
    if block[SQUASH_TYPE] == SQUASH_ZLIB:
        return zlib.decompress(payload)
    raise SystemExit("the save holds a block this check cannot read")


def pool_name(pool, offset):
    return pool[offset:pool.index(b"\0", offset)].decode("latin-1")


def summary_max_units(image):
    """The pool, the Summary account's record span and payload, and its maxunits entry offset."""
    if image[:8] != BANK_MAGIC:
        raise SystemExit("not a saved-game bank")
    pool_offset, = struct.unpack_from("<I", image, BANK_POOL_OFFSET)
    pool = image[pool_offset:]
    if image[BANK_POOL_PACKED]:
        pool = unsquash(pool)
    position, = struct.unpack_from("<I", image, BANK_FIRST_ACCOUNT)
    while position < pool_offset:
        size, name = struct.unpack_from("<II", image, position + ACCOUNT_SIZE)
        ints, = struct.unpack_from("<I", image, position + ACCOUNT_COUNTS)
        if pool_name(pool, name).lower() == SUMMARY_ACCOUNT:
            payload = image[position + ACCOUNT_HEADER_BYTES:position + size]
            packed, = struct.unpack_from("<I", image, position + ACCOUNT_PACKED)
            if packed == 1:
                payload = unsquash(payload)
            for entry in range(0, ints * INT_ENTRY_BYTES, INT_ENTRY_BYTES):
                field, = struct.unpack_from("<I", payload, entry)
                if pool_name(pool, field) == MAX_UNITS_FIELD:
                    return pool_offset, position, size, bytearray(payload), entry
            return pool_offset, position, size, bytearray(payload), None
        position += size
    raise SystemExit("the save has no Summary account")


def saved_unit_limit(path):
    """The Summary maxunits a save holds, or None."""
    _, _, _, payload, entry = summary_max_units(path.read_bytes())
    return None if entry is None else struct.unpack_from("<i", payload, entry + 4)[0]


def edit_unit_limit(source, target, limit):
    """Writes `source` to `target` with its Summary maxunits set to `limit`, or without the
    field when `limit` is None (its entry then names "axunits", which nothing reads).

    Blob entries hold file positions, so no account moves: the original Summary record is
    renamed "ummary", which nothing reads, and the edited one, unpacked, follows the last
    account."""
    image = bytearray(source.read_bytes())
    pool_offset, position, size, payload, entry = summary_max_units(bytes(image))
    if entry is None:
        raise SystemExit(f"{source.name} has no Summary maxunits")
    if limit is None:
        field, = struct.unpack_from("<I", payload, entry)
        struct.pack_into("<I", payload, entry, field + 1)
    else:
        struct.pack_into("<i", payload, entry + 4, limit)
    header = bytearray(image[position:position + ACCOUNT_HEADER_BYTES])
    name, = struct.unpack_from("<I", header, ACCOUNT_NAME)
    struct.pack_into("<I", image, position + ACCOUNT_NAME, name + 1)
    ints, reals, texts, blobs = struct.unpack_from("<IIII", header, ACCOUNT_COUNTS)
    table = ints * INT_ENTRY_BYTES + reals * REAL_ENTRY_BYTES + texts * TEXT_ENTRY_BYTES
    for blob in range(table, table + blobs * BLOB_ENTRY_BYTES, BLOB_ENTRY_BYTES):
        data, = struct.unpack_from("<I", payload, blob + BLOB_DATA_OFFSET)
        struct.pack_into("<I", payload, blob + BLOB_DATA_OFFSET, data - position + pool_offset)
    struct.pack_into("<I", header, ACCOUNT_SIZE, ACCOUNT_HEADER_BYTES + len(payload))
    struct.pack_into("<I", header, ACCOUNT_PACKED, 0)
    edited = image[:pool_offset] + header + payload + image[pool_offset:]
    struct.pack_into("<I", edited, BANK_POOL_OFFSET, pool_offset + len(header) + len(payload))
    target.write_bytes(bytes(edited))


def launch(native, game_dir, profile, cwd, *extra):
    return subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(profile), *extra],
        cwd=cwd, timeout=RUN_TIMEOUT_SECONDS, check=False, capture_output=True, text=True,
    )


def run(native, game_dir, profile, cwd, *extra):
    result = launch(native, game_dir, profile, cwd, *extra)
    print(result.stdout, end="")
    if result.returncode != 0:
        print(result.stderr, end="")
        raise SystemExit(f"open-annihilation exited with {result.returncode}")
    match = DIGEST.search(result.stdout)
    if match is None:
        print(result.stderr, end="")
        raise SystemExit("no saveload digest line in the output")
    orders = ORDERS.search(result.stdout)
    if orders is None:
        raise SystemExit("no saveload orders line in the output")
    missions = dict(item.split("=") for item in orders.group(2).split())
    features = FEATURES.search(result.stdout)
    if features is None:
        raise SystemExit("no saveload features line in the output")
    limits = LIMITS.search(result.stdout)
    if limits is None:
        raise SystemExit("no saveload unit limit line in the output")
    reports = FAILURES.findall(result.stdout)
    if not reports:
        raise SystemExit("no save or restore failure count in the output")
    for kind, count in reports:
        if int(count) != 0:
            raise SystemExit(f"{kind} dropped state of {count} unit parts")
    return (int(match.group(1)), int(match.group(2)), match.group(3), int(orders.group(1)),
            missions, features.groups(), tuple(int(limit) for limit in limits.groups()))


# The engine's registry, from which the baseline profile is written.
REGISTRY = Path(__file__).resolve().parent.parent / "src/data/mod-profile/registry/hack-registry.yaml"
# What a run prints when it refuses a save of another profile.
REFUSED_PROFILE = "Savegame belongs to mod profile baseline-rules"


def baseline_profile(target):
    """Writes the profile of every limit and hack with a baseline preset, at that preset."""
    spec = importlib.util.spec_from_file_location(
        "oamod_yaml", Path(__file__).resolve().parent / "oamod_yaml.py")
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    registry = reader.load_file(str(REGISTRY))
    limits, hacks = [], []
    for name, entry in registry["entries"].items():
        if "baseline" not in (entry.get("presets") or {}):
            continue
        if entry.get("kind") == "limit" and name.startswith("limits."):
            limits.append(f"  {name[len('limits.'):]}: {{preset: baseline}}\n")
        elif entry.get("kind") == "hack":
            hacks.append(f"  {name}: {{preset: baseline}}\n")
    text = ("oamod: 1\nid: baseline-rules\nname: \"Every rule at its baseline\"\nversion: \"1\"\n"
            f"requires: {{base: ta-3.1c, catalogue: {registry['catalogue']}}}\n"
            "author: {name: Open Annihilation}\n"
            "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n")
    if limits:
        text += "limits:\n" + "".join(limits)
    if hacks:
        text += "hacks:\n" + "".join(hacks)
    target.write_text(text)


def check_baseline_profile(native, game_dir, cwd, save, saved):
    """Plays run A under the baseline profile and moves saves across it."""
    mod = Path(cwd) / "baseline-rules.yaml"
    baseline_profile(mod)
    under = ("--mod", str(mod), "--accept-unimplemented-hacks")
    profile = Path(cwd) / "baseline-preferences.conf"
    mod_save = Path(cwd) / "savegame" / "baseline-rules.sav"
    played = run(native, game_dir, profile, cwd, *under, "--combat", str(COMBAT_UNITS),
                 "--give-orders", "--match-ticks", str(SAVE_TICK), "--save-after", str(SAVE_TICK),
                 "--save-file", str(mod_save))
    if played[:6] != saved[:6]:
        raise SystemExit(f"run A under the baseline profile saved digest {played[2]} with "
                         f"{played[1]} units, not {saved[2]} with {saved[1]}")
    refused = launch(native, game_dir, profile, cwd, "--load", str(mod_save), "--match-ticks", "0")
    if refused.returncode == 0 or REFUSED_PROFILE not in refused.stderr + refused.stdout:
        print(refused.stdout + refused.stderr, end="")
        raise SystemExit("a save of the baseline profile loaded without it")
    loaded = run(native, game_dir, profile, cwd, *under, "--load", str(mod_save),
                 "--match-ticks", "0")
    plain = run(native, game_dir, profile, cwd, *under, "--load", str(save), "--match-ticks", "0")
    if loaded[:6] != saved[:6] or plain[:6] != saved[:6]:
        raise SystemExit(f"under the baseline profile its save loads to digest {loaded[2]} and "
                         f"run A's to {plain[2]}, not {saved[2]}")
    print(f"saveload check: the baseline profile plays and loads digest {saved[2]}; its save "
          f"is refused without it")


def check_unit_limits(native, game_dir, profile, cwd, save, saved):
    """Loads run A's save edited to no limit, another limit and one no pool holds."""
    if saved[6] != (DEFAULT_UNIT_LIMIT,) * 3 or saved_unit_limit(save) != DEFAULT_UNIT_LIMIT:
        raise SystemExit(f"run A played at limits {saved[6]}, saving maxunits "
                         f"{saved_unit_limit(save)}, not {DEFAULT_UNIT_LIMIT}")
    # The run-wide limit is the default in a fresh run.
    unset = save.with_name("no-limit.sav")
    edit_unit_limit(save, unset, None)
    if saved_unit_limit(unset) is not None:
        raise SystemExit("the edited save still holds maxunits")
    loaded = run(native, game_dir, profile, cwd, "--load", str(unset), "--match-ticks", "0")
    if loaded != saved:
        raise SystemExit(f"a save without maxunits loads at limits {loaded[6]}, digest "
                         f"{loaded[2]}, not run A's {saved[6]}, {saved[2]}")

    other = save.with_name("other-limit.sav")
    edit_unit_limit(save, other, OTHER_UNIT_LIMIT)
    resaved = save.with_name("other-limit-resaved.sav")
    built = run(native, game_dir, profile, cwd, "--load", str(other), "--match-ticks", "0",
                "--save-after", str(SAVE_TICK), "--save-file", str(resaved))
    if built[6] != (OTHER_UNIT_LIMIT,) * 3:
        raise SystemExit(f"a save at maxunits {OTHER_UNIT_LIMIT} loads at limits {built[6]}")
    if built[:6] != saved[:6]:
        raise SystemExit(f"a save at maxunits {OTHER_UNIT_LIMIT} loads digest {built[2]} "
                         f"units {built[1]}, not run A's {saved[2]} units {saved[1]}")
    if saved_unit_limit(resaved) != OTHER_UNIT_LIMIT:
        raise SystemExit(f"the game saved maxunits {saved_unit_limit(resaved)} from a match at "
                         f"{OTHER_UNIT_LIMIT}")
    reloaded = run(native, game_dir, profile, cwd, "--load", str(resaved), "--match-ticks", "0")
    if reloaded != built:
        raise SystemExit(f"the game's save at {OTHER_UNIT_LIMIT} loads at limits {reloaded[6]} "
                         f"digest {reloaded[2]}, not {built[6]} {built[2]}")
    resumed = run(native, game_dir, profile, cwd, "--load", str(resaved),
                  "--match-ticks", str(RESUME_TICKS))
    if resumed[0] != SAVE_TICK + RESUME_TICKS or resumed[6] != built[6]:
        raise SystemExit(f"the match at {OTHER_UNIT_LIMIT} resumed to tick {resumed[0]} at "
                         f"limits {resumed[6]}")

    unbuildable = save.with_name("unbuildable-limit.sav")
    edit_unit_limit(save, unbuildable, UNBUILDABLE_UNIT_LIMIT)
    refused = launch(native, game_dir, profile, cwd, "--load", str(unbuildable),
                     "--match-ticks", "0")
    if refused.returncode == 0 or REFUSED_LIMIT not in refused.stderr + refused.stdout:
        print(refused.stdout + refused.stderr, end="")
        raise SystemExit(f"a save at maxunits {UNBUILDABLE_UNIT_LIMIT} was not refused")
    print(f"saveload check: saves load at their own limit: none at {DEFAULT_UNIT_LIMIT}, "
          f"{OTHER_UNIT_LIMIT} at {OTHER_UNIT_LIMIT} through a save and a load, "
          f"{UNBUILDABLE_UNIT_LIMIT} refused")


def check_setting_unit_limit(native, game_dir, cwd):
    """Plays run A with the Unit limit setting at SETTING_UNIT_LIMIT, then loads its save in a
    run whose setting is the default."""
    scratch = Path(cwd)
    profile = scratch / "unit-limit-setting.conf"
    profile.write_text(f'{PREFERENCES_HEADER}\n"{UNIT_LIMIT_KEY}" "{SETTING_UNIT_LIMIT}"\n')
    save = scratch / "savegame" / "unit-limit-setting.sav"
    saved = run(native, game_dir, profile, cwd, "--combat", str(COMBAT_UNITS), "--give-orders",
                "--match-ticks", str(SAVE_TICK), "--save-after", str(SAVE_TICK),
                "--save-file", str(save))
    expected = (SETTING_UNIT_LIMIT,) * 3
    if saved[0] != SAVE_TICK or saved[6] != expected:
        raise SystemExit(f"a skirmish with the setting at {SETTING_UNIT_LIMIT} played to tick "
                         f"{saved[0]} at limits {saved[6]}")
    if saved_unit_limit(save) != SETTING_UNIT_LIMIT:
        raise SystemExit(f"a skirmish at {SETTING_UNIT_LIMIT} saved maxunits "
                         f"{saved_unit_limit(save)}")
    fresh = scratch / "unit-limit-default.conf"
    fresh.write_text(f"{PREFERENCES_HEADER}\n")
    loaded = run(native, game_dir, fresh, cwd, "--load", str(save), "--match-ticks", "0")
    if loaded != saved:
        raise SystemExit(f"the save at {SETTING_UNIT_LIMIT} loads at limits {loaded[6]} digest "
                         f"{loaded[2]} units {loaded[1]}, not {saved[6]} {saved[2]} {saved[1]}")
    resumed = run(native, game_dir, fresh, cwd, "--load", str(save),
                  "--match-ticks", str(RESUME_TICKS))
    if resumed[0] != SAVE_TICK + RESUME_TICKS or resumed[6] != expected:
        raise SystemExit(f"the match at {SETTING_UNIT_LIMIT} resumed to tick {resumed[0]} at "
                         f"limits {resumed[6]}")
    print(f"saveload check: a skirmish at the setting's {SETTING_UNIT_LIMIT} saves and loads at "
          f"it into the same world, digest {saved[2]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    scratch_root = args.scratch_root.resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-saveload-", dir=scratch_root) as temporary:
        profile = Path(temporary) / "preferences.conf"
        save = Path(temporary) / "savegame" / SAVE_NAME
        saved = run(native, game_dir, profile, temporary, "--combat", str(COMBAT_UNITS),
                    "--give-orders", "--match-ticks", str(SAVE_TICK), "--save-after", str(SAVE_TICK),
                    "--save-file", str(save))
        if saved[0] != SAVE_TICK or not save.is_file():
            raise SystemExit(f"run A did not save at tick {SAVE_TICK}")
        if saved[1] == 0:
            raise SystemExit("run A saved a world without units")
        burning, dying, reclaimed = (int(count) for count in saved[5][3:6])
        if burning == 0 or dying == 0 or reclaimed == 0:
            raise SystemExit(
                f"run A saved {burning} burning, {dying} dying and {reclaimed} reclaimed features")
        missing = SAVED_MISSIONS - saved[4].keys()
        if missing:
            raise SystemExit(f"run A saved no {', '.join(sorted(missing))} orders")
        if int(saved[4]["BeCarried"]) < TRANSPORTED:
            raise SystemExit(f"run A saved {saved[4]['BeCarried']} carried units, not the "
                             f"{TRANSPORTED} aboard the Atlas and the transport ship")
        loaded = run(native, game_dir, profile, temporary, "--load", str(save), "--match-ticks", "0")
        if loaded != saved:
            raise SystemExit(
                f"loaded world differs: saved tick {saved[0]} units {saved[1]} digest {saved[2]} "
                f"orders {saved[3]} {saved[4]} features {saved[5]}, loaded tick {loaded[0]} "
                f"units {loaded[1]} digest {loaded[2]} orders {loaded[3]} {loaded[4]} "
                f"features {loaded[5]}")
        resumed = run(native, game_dir, profile, temporary,
                      "--load", str(save), "--match-ticks", str(RESUME_TICKS))
        if resumed[0] != SAVE_TICK + RESUME_TICKS or resumed[1] == 0:
            raise SystemExit(f"resumed match did not reach tick {SAVE_TICK + RESUME_TICKS}")
        dropped = RESUMED_MISSIONS - resumed[4].keys()
        if dropped:
            raise SystemExit(f"resumed match lost its {', '.join(sorted(dropped))} orders")
        if int(resumed[4]["BeCarried"]) < TRANSPORTED:
            raise SystemExit(f"resumed match carries {resumed[4]['BeCarried']} units, not the "
                             f"{TRANSPORTED} aboard the Atlas and the transport ship")
        check_unit_limits(native, game_dir, profile, temporary, save, saved)
        check_baseline_profile(native, game_dir, temporary, save, saved)
        check_setting_unit_limit(native, game_dir, temporary)
        print(f"saveload check: tick {SAVE_TICK} digest {saved[2]} with {saved[1]} units "
              f"({saved[4]['BeCarried']} carried), {saved[3]} orders and features {saved[5][6]} "
              f"({saved[5][2]} animating) matches after load; resumed to tick {resumed[0]} with "
              f"{resumed[1]} units and {resumed[3]} orders")
        for campaign, mission, carriers in CARRIED_MISSIONS:
            carried_save = Path(temporary) / "savegame" / f"carried{mission}.sav"
            carried = run(native, game_dir, profile, temporary, "--campaign", campaign,
                          "--mission", str(mission), "--match-ticks", str(CARRIED_SAVE_TICK),
                          "--save-after", str(CARRIED_SAVE_TICK), "--save-file", str(carried_save))
            if carried[0] != CARRIED_SAVE_TICK or not carried_save.is_file():
                raise SystemExit(f"{campaign.strip()} mission {mission} did not save")
            aboard = int(carried[4].get("BeCarried", 0))
            if aboard == 0:
                raise SystemExit(f"{campaign.strip()} mission {mission} saved no units aboard "
                                 f"{carriers}")
            reloaded = run(native, game_dir, profile, temporary, "--load", str(carried_save),
                           "--match-ticks", "0")
            if reloaded != carried:
                raise SystemExit(
                    f"{campaign.strip()} mission {mission} with units aboard {carriers} differs "
                    f"after load: saved digest {carried[2]} orders {carried[4]} features "
                    f"{carried[5]}, loaded digest {reloaded[2]} orders {reloaded[4]} features "
                    f"{reloaded[5]}")
            # A campaign save plays at its mission's limit; its maxunits sets only the
            # run-wide limit.
            other = carried_save.with_name(f"carried{mission}-other-limit.sav")
            edit_unit_limit(carried_save, other, CAMPAIGN_SAVE_UNIT_LIMIT)
            edited = run(native, game_dir, profile, temporary, "--load", str(other),
                         "--match-ticks", "0")
            expected = (carried[6][0], CAMPAIGN_SAVE_UNIT_LIMIT, CAMPAIGN_SAVE_UNIT_LIMIT)
            if edited[:6] != carried[:6] or edited[6] != expected:
                raise SystemExit(
                    f"{campaign.strip()} mission {mission} saved at maxunits "
                    f"{CAMPAIGN_SAVE_UNIT_LIMIT} loads at limits {edited[6]} digest {edited[2]}, "
                    f"not {expected} {carried[2]}")
            print(f"saveload check: {aboard} units aboard {carriers} ({campaign.strip()} mission "
                  f"{mission}) match after load, digest {carried[2]}, features {carried[5][6]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
