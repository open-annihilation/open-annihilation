#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check shape cues on a staged ten-player battle.

One peewee of each player stands in a row, far enough apart that the minimap
markers do not touch, and the local player's peewee is selected. A second
peewee of that player stands on screen and is not selected. The picture with
shape cues off is compared with the picture with them on. Each player's
minimap dot gains a marker, the selected peewee gains one, and the unselected
peewee stays as it was. Both runs save the battle, and the saves match.
Each run writes its own preferences file and user folder under a fresh
scratch directory.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import native_offline

# A command that starts the executable under test; empty runs it directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

WIDTH = 640
HEIGHT = 480
PLAYERS = 10
ORIGIN_X = 128
ORIGIN_Y = 32
CAMERA_X = 400
CAMERA_Z = 1500
UNIT_Z = 1700
UNIT_X0 = 432
# Canal Crossing's radar is 126 pixels across a map about 8800 wide, so a
# step of 640 leaves about nine pixels between dots. A compact marker is
# seven pixels, and the next player's marker starts clear of it.
UNIT_STEP = 640
# A second peewee of player 1, on screen and not selected. It shares that
# army, so its minimap dot joins the same marker.
COMPANION_DX = 100
SIDE_COLUMN = 128
# A dot is a few pixels. A marker is wider, so the search reaches past it.
BLIP_SPAN = 24
MARKER_PAD = 24
CHANGED_PIXELS = 8
# The selected peewee's marker sits at the corner of its selection box,
# above and to the left of the unit. This reach takes in that corner.
UNIT_WINDOW = 72
# The unselected peewee stands above its foot. This band holds that sprite
# and stays clear of the selected peewee a hundred map units to the left.
COMPANION_HALF_X = 24
COMPANION_ABOVE = 80
COMPANION_BELOW = 16
TIMEOUT = 280
BLACK = b"\x00\x00\x00"
WHITE = b"\xff\xff\xff"

# Stock palette RGB, index order, the unused fourth byte of each entry dropped.
PALETTE = bytes.fromhex(
    "000000800000008000808000000080800080008080808080c0dcc05454fc000000000000000000000000"
    "000000000000ffebf3ebc7d3d7a3b3c38797af6f7f9b5b638b474f7b3b476f333b632b3357232b4b1b27"
    "3b171f2f0f17230b0f17070b73ffdf57e7bf43cf9f2fb7831f9f67138b4f0f773f0b6b37075f2f07532b"
    "004727003f2300331b002717001b0f00130be3efffc7dfe7afcfcb93b7a77f9f836b87675f6f535f6347"
    "5b573b534333473b2b3b33232f2b1b231f1317130f0b0b07fbfbd7dfdfb7c3c39babab8393936f777757"
    "63634353533343432333331723230f1b1b071717071313000f0f000b0b00fbfbfbebebebdbdbdbcbcbcb"
    "bbbbbbababab9b9b9b8b8b8b7b7b7b6b6b6b5b5b5b4b4b4b3b3b3b2b2b2b1f1f1f0f0f0febf3ffcbe3ff"
    "afcfff97b3ff7b97ff677fff536bef3f5be3334bd7233bcb172faf0f2797071f7b071763000f47000b2f"
    "e3f7ffbfdbe79fbfcf83a7b76b8fa353778b3f5f732f4b5f273f5723374f1f2f471b273f171f37131b2f"
    "0f13270b0f1fd7efffbbe3ef9bcbdf83b7cf6ba3c3538fb33f7ba32f6b97235b871b4b77133f670b3357"
    "072747001b37001327000b1bffe7ffe7c7ebd3abd7bb93c3a77bb38f639f774b8f633b7f4f2b6f431f63"
    "3717572b0f471f073b13002b0b001f070013d7ffa7abe77f83d35b67bf3f4bab2b43972b3787272f771b"
    "2b6713235b0f1f4f0b1b43071733000f27000b1b00070f00ffe39fe3c773cbaf53b3973f9b832f836f23"
    "6b5b1753470f4b3b0b4333073b2b073723002f1b002713001f0f001b0b00ffffa3fbf383f7e367f3d34f"
    "efbb33efa71beb8f13e77b0fdf4f07d72300bf1f00a71b009317007b13006313004f0f00ffff00ffbf00"
    "ff8300ff4700d32b00ab17007f0700570000dfcbffbb9fdf9b77bf7f579f673b7f4b235f33133f1b071f"
    "d3dbff879ff7436fef1747e70b2bbb07178f0007630000377bff7753df4f33bf2b1b9f131b7f0b175f07"
    "133f000b1f00000000000000000000000000000000000000fffbf0a0a0a4808080ff000000ff00ffff00"
    "0000ffff00ff00ffffffffff"
)
# The blip body of each player colour, the index the minimap logo draws unshaded.
BODIES = (227, 212, 255, 235, 108, 218, 208, 245, 131, 67)
# The marker's colour, the dot drawn in place of that logo.
DOTS = (227, 212, 80, 235, 108, 219, 208, 93, 130, 67)
PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player (\d+) at (-?\d+),(-?\d+)$", re.M)
SELECTED = re.compile(r"^stage: select own: (\d+) units$", re.M)


def rgb(index):
    at = index * 3
    return PALETTE[at:at + 3]


BODY_RGB = tuple(rgb(index) for index in BODIES)
DOT_RGB = tuple(rgb(index) for index in DOTS)


def quoted(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def preferences(path, cues):
    """Writes one fresh preferences file. The skirmish section keeps one backslash."""
    rows = [
        ("Total Annihilation|NumSkirmishPlayers", "10"),
        ("open-annihilation.hardware-acceleration", "off"),
        ("open-annihilation.access.colour-vision", "off"),
        ("open-annihilation.access.shape-cues", cues),
    ]
    section = "Total Annihilation\\Skirmish|"
    for player in range(PLAYERS):
        rows.append((section + f"Player{player}Controller", "1" if player == 0 else "2"))
        rows.append((section + f"Player{player}Color", str(player)))
        rows.append((section + f"Player{player}AllyGroup", "5"))
        rows.append((section + f"Player{player}Side", str(player % 2)))
    lines = ["open-annihilation-preferences 1",
            '"open-annihilation.update-check" "never"',
            '"open-annihilation.content-updates" "never"']
    lines.extend(quoted(key) + " " + quoted(value) for key, value in rows)
    path.write_text("\n".join(lines) + "\n")


def stage_text():
    # Full radar, so every army's dot is on the minimap. Only the local player's
    # first peewee is selected. The second is placed after the group changes,
    # so select own leaves it out, and it stands on screen.
    lines = ["console +radar", "group own", f"place 0 ARMPW {UNIT_X0} {UNIT_Z} south", "group rest"]
    lines.append(f"place 0 ARMPW {UNIT_X0 + COMPANION_DX} {UNIT_Z} south")
    for player in range(1, PLAYERS):
        lines.append(f"place {player} ARMPW {UNIT_X0 + player * UNIT_STEP} {UNIT_Z} south")
    lines.append("select own")
    return "\n".join(lines) + "\n"


def run(native, game_dir, workdir, label, cues):
    folder = workdir / label
    folder.mkdir()
    preferences(folder / "preferences.conf", cues)
    user = folder / "user"
    user.mkdir()
    stage = folder / "armies.stage"
    stage.write_text(stage_text())
    snapshot = folder / "battle.ppm"
    save = folder / "battle.sav"
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
         "--headless-check", "--match-ticks", "1", "--save-after", "1", "--save-file", str(save),
         "--seed", "1", "--resolution", "640x480",
         "--camera", f"{CAMERA_X},{CAMERA_Z}", "--stage", str(stage), "--snapshot", str(snapshot),
         "--preferences-file", str(folder / "preferences.conf"), "--user-folder", str(user)],
        cwd=folder,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=TIMEOUT, check=False, capture_output=True, text=True, errors="replace")
    return result, snapshot, save


def fail(result, message):
    if result is not None:
        text = result.stdout + result.stderr
        tail = text[-4000:]
        print(tail, end="" if tail.endswith("\n") or not tail else "\n")
    raise SystemExit(message)


def read_ppm(path):
    data = path.read_bytes()
    if not data.startswith(b"P6\n"):
        return None
    _header, _, pixels = data.partition(b"\n")
    size, _, pixels = pixels.partition(b"\n")
    depth, _, pixels = pixels.partition(b"\n")
    try:
        width, height = (int(part) for part in size.split())
    except ValueError:
        return None
    if depth != b"255" or width != WIDTH or height != HEIGHT or len(pixels) != width * height * 3:
        return None
    return pixels


def pixel(frame, x, y):
    at = (y * WIDTH + x) * 3
    return frame[at:at + 3]


def components(points):
    """Connected groups of pixels, each as a list of (x, y)."""
    remaining = set(points)
    groups = []
    while remaining:
        start = remaining.pop()
        group = [start]
        stack = [start]
        while stack:
            x, y = stack.pop()
            for next_x, next_y in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                neighbour = (next_x, next_y)
                if neighbour in remaining:
                    remaining.remove(neighbour)
                    group.append(neighbour)
                    stack.append(neighbour)
        groups.append(group)
    return groups


def span(group):
    xs = [x for x, _ in group]
    ys = [y for _, y in group]
    return max(xs) - min(xs) + 1, max(ys) - min(ys) + 1


def check_players(output):
    placed = [
        (name, int(player), int(x), int(z))
        for _, name, player, x, z in PLACED.findall(output)
    ]
    expected = [
        ("ARMPW", 0, UNIT_X0, UNIT_Z),
        ("ARMPW", 0, UNIT_X0 + COMPANION_DX, UNIT_Z),
    ]
    expected.extend(
        ("ARMPW", player, UNIT_X0 + player * UNIT_STEP, UNIT_Z) for player in range(1, PLAYERS)
    )
    if placed != expected:
        return f"the stage placed {placed}"
    selected = [int(count) for count in SELECTED.findall(output)]
    if selected != [1]:
        return f"the stage selected {selected}"
    return None


def unit_centre(player):
    return (
        ORIGIN_X + (UNIT_X0 + player * UNIT_STEP - CAMERA_X),
        ORIGIN_Y + (UNIT_Z - CAMERA_Z),
    )


def dots(frame, body):
    """Compact body-coloured groups in the minimap, the square at the top of the side column."""
    points = [
        (x, y)
        for y in range(SIDE_COLUMN)
        for x in range(SIDE_COLUMN)
        if pixel(frame, x, y) == body
    ]
    groups = []
    for group in components(points):
        wide, tall = span(group)
        if wide <= BLIP_SPAN and tall <= BLIP_SPAN:
            groups.append(group)
    return groups


def gains_marker(off, on, group, pad, ink):
    """The dot colour appears in the 7x7 around the dot, with a black or white outline."""
    xs = [x for x, _ in group]
    ys = [y for _, y in group]
    centre_x = (min(xs) + max(xs)) // 2
    centre_y = (min(ys) + max(ys)) // 2
    stays = False
    for y in range(centre_y - 3, centre_y + 4):
        for x in range(centre_x - 3, centre_x + 4):
            if 0 <= x < WIDTH and 0 <= y < HEIGHT and pixel(on, x, y) == ink:
                stays = True
    x0 = max(0, min(xs) - pad)
    y0 = max(0, min(ys) - pad)
    x1 = min(WIDTH, max(xs) + pad + 1)
    y1 = min(HEIGHT, max(ys) + pad + 1)
    changed = 0
    new_outline = False
    for y in range(y0, y1):
        for x in range(x0, x1):
            before = pixel(off, x, y)
            after = pixel(on, x, y)
            if before != after:
                changed += 1
            if (after == BLACK and before != BLACK) or (after == WHITE and before != WHITE):
                new_outline = True
    return stays and new_outline and changed >= CHANGED_PIXELS


def saves_differ(off_path, on_path):
    """None when the two saves are the same bytes."""
    if not off_path.is_file() or not on_path.is_file():
        return (
            f"a save is missing (cues off {off_path.is_file()}, cues on {on_path.is_file()})"
        )
    off_bytes = off_path.read_bytes()
    on_bytes = on_path.read_bytes()
    if off_bytes == on_bytes:
        return None
    limit = min(len(off_bytes), len(on_bytes))
    at = next((index for index in range(limit) if off_bytes[index] != on_bytes[index]), limit)
    return f"the saves differ at byte {at} ({len(off_bytes)} and {len(on_bytes)} bytes)"


def window_changed(off, on, centre_x, centre_y, reach):
    changed = 0
    new_black = False
    body = False
    for y in range(max(0, centre_y - reach), min(HEIGHT, centre_y + reach + 1)):
        for x in range(max(0, centre_x - reach), min(WIDTH, centre_x + reach + 1)):
            before = pixel(off, x, y)
            after = pixel(on, x, y)
            if before != after:
                changed += 1
            if after == BLACK and before != BLACK:
                new_black = True
            if after == BODY_RGB[0]:
                body = True
    return changed, new_black, body


def companion_unchanged(off, on, centre_x, centre_y):
    """The unselected peewee is on screen, and that window did not change.

    The foot is the map point. The sprite stands above it. The window has to
    hold the player's colour, or it would pass while looking at empty ground.
    """
    x0 = max(0, centre_x - COMPANION_HALF_X)
    x1 = min(WIDTH, centre_x + COMPANION_HALF_X + 1)
    y0 = max(0, centre_y - COMPANION_ABOVE)
    y1 = min(HEIGHT, centre_y + COMPANION_BELOW + 1)
    held = 0
    same = True
    for y in range(y0, y1):
        for x in range(x0, x1):
            before = pixel(off, x, y)
            if before != pixel(on, x, y):
                same = False
            if before == BODY_RGB[0]:
                held += 1
    return same and held >= 3


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    arguments.native = arguments.native.absolute()
    arguments.game_dir = arguments.game_dir.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    if len(PALETTE) != 256 * 3:
        raise SystemExit("the stock palette table is not 256 colours")
    with tempfile.TemporaryDirectory(prefix="A08-", dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        off_result, off_snapshot, off_save = run(
            arguments.native, arguments.game_dir, workdir, "off", "0"
        )
        if off_result.returncode != 0:
            fail(off_result, f"the battle with shape cues off exited {off_result.returncode}")
        placed = check_players(off_result.stdout + off_result.stderr)
        if placed:
            fail(off_result, placed)
        off = read_ppm(off_snapshot)
        if off is None:
            fail(off_result, "the cues-off snapshot is not a 640x480 picture")
        on_result, on_snapshot, on_save = run(
            arguments.native, arguments.game_dir, workdir, "on", "1"
        )
        if on_result.returncode != 0:
            fail(on_result, f"the battle with shape cues on exited {on_result.returncode}")
        placed = check_players(on_result.stdout + on_result.stderr)
        if placed:
            fail(on_result, placed)
        shown = read_ppm(on_snapshot)
        if shown is None:
            fail(on_result, "the cues-on snapshot is not a 640x480 picture")
        problems = []
        differed = saves_differ(off_save, on_save)
        if differed:
            problems.append(differed)
        for player, body in enumerate(BODY_RGB):
            groups = dots(off, body)
            if not groups:
                problems.append(f"player {player + 1} minimap has no dot")
                continue
            if not any(
                gains_marker(off, shown, group, MARKER_PAD, DOT_RGB[player]) for group in groups
            ):
                problems.append(f"player {player + 1} minimap dot gained no marker")
        centre_x, centre_y = unit_centre(0)
        changed, new_black, body_there = window_changed(off, shown, centre_x, centre_y, UNIT_WINDOW)
        if changed < CHANGED_PIXELS or not new_black or not body_there:
            problems.append(
                f"the selected unit at {centre_x},{centre_y} changed {changed} pixels, "
                f"new black {new_black}, player colour {body_there}"
            )
        bare_x, bare_y = unit_centre(0)
        bare_x += COMPANION_DX
        if not companion_unchanged(off, shown, bare_x, bare_y):
            problems.append(
                f"the unselected peewee at {bare_x},{bare_y} changed, or it is not on screen"
            )
        if problems:
            print("\n".join(problems))
            fail(on_result, "shape cues did not mark the minimap dots and the selected unit")
    print(
        "shape cues check: each player's minimap dot and the selected unit "
        "carry a marker, the other units stay as they were, and the two saves match"
    )


if __name__ == "__main__":
    main()
