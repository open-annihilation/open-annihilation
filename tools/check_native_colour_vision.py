#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that each colour-vision mode recolours a ten-player battle.

A headless skirmish seats ten players from a fresh preferences file and
stages one peewee of each player in a row. The picture with the preference
off is compared with each mode. Each player's minimap blip becomes that
player's anchor colour, and that player's peewee leaves the old team ramp.
Most of the picture stays as it was: a mode is not a tint of the whole screen.
Army distance is measured on the unit pixels a mode recolours, under Machado
and under Viénot or Brettel.
"""
import argparse
import math
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through; empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

WIDTH = 640
HEIGHT = 480
PLAYERS = 10
# The battlefield's top-left in the 640x480 picture, and the camera's map cell.
ORIGIN_X = 128
ORIGIN_Y = 32
CAMERA_X = 400
CAMERA_Z = 1500
UNIT_Z = 1700
# The map's land ends near this screen column at the camera above, so the row
# stays on the land. A peewee's team colours are a few pixels wide.
UNIT_X0 = 432
UNIT_STEP = 24
UNIT_HALF_X = 12
UNIT_ABOVE = 80
UNIT_BELOW = 36
SIDE_COLUMN = 128
# A blip is a few pixels of the logo. One pixel is still that player's dot.
BLIP_SPAN = 24
BLIP_PIXELS = 1
UNIT_ANCHOR_PIXELS = 3
UNIT_CHANGED_PIXELS = 12
# A tint of the whole picture would move far more than the ten armies.
CHANGED_FRACTION = 0.08
BLOCK = 16
TIMEOUT = 280

# Stock palette RGB, index order, the unused fourth byte of each entry dropped.
# The classic picture draws these channel values for a palette index.
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

# Indices of each player colour's old team ramp, the shades logos.gaf draws.
SHADES = (
    list(range(98, 112)) + [227],
    [75, 188, 189] + list(range(198, 208)) + [212, 249],
    list(range(80, 90)) + list(range(91, 94)) + [255],
    list(range(162, 174)) + list(range(234, 240)),
    list(range(106, 112)) + [230, 231, 245],
    [141, 142] + list(range(147, 158)) + list(range(218, 224)),
    list(range(192, 204)) + [208, 209],
    list(range(91, 96)) + [245],
    list(range(128, 142)),
    list(range(64, 76)),
)
# The blip body of each player colour, the index the minimap draws unshaded.
BODIES = (227, 212, 255, 235, 108, 218, 208, 245, 131, 67)
ANCHORS = {
    "protanopia": (228, 85, 64, 54, 123, 117, 208, 186, 225, 179),
    "deuteranopia": (117, 199, 64, 236, 229, 225, 208, 215, 113, 52),
    "tritanopia": (100, 213, 80, 167, 187, 219, 199, 172, 32, 67),
    "achromatopsia": (29, 55, 164, 232, 29, 55, 164, 232, 29, 55),
}
# Slot 14, the radar's marks (shot dots and the view rectangle). Off is 194.
MARKS = {"protanopia": 64, "deuteranopia": 64, "tritanopia": 193, "achromatopsia": 86}
MODES = ("protanopia", "deuteranopia", "tritanopia", "achromatopsia")
PLACED = re.compile(r"^stage: unit (\d+) (\S+) of player (\d+) at (-?\d+),(-?\d+)$", re.M)


def rgb(index):
    at = index * 3
    return PALETTE[at:at + 3]


SHADE_RGB = tuple(frozenset(rgb(index) for index in shades) for shades in SHADES)
BODY_RGB = tuple(rgb(index) for index in BODIES)
ANCHOR_RGB = {mode: tuple(rgb(index) for index in indices) for mode, indices in ANCHORS.items()}
MARK_OFF = 194
MARK_OFF_RGB = rgb(MARK_OFF)
MARK_RGB = {mode: rgb(index) for mode, index in MARKS.items()}


def quoted(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def preferences(path, mode):
    """Writes one fresh preferences file. The skirmish section keeps one backslash."""
    rows = [
        ("Total Annihilation|NumSkirmishPlayers", "10"),
        ("open-annihilation.hardware-acceleration", "off"),
        ("open-annihilation.access.colour-vision", mode),
    ]
    section = "Total Annihilation\\Skirmish|"
    for player in range(PLAYERS):
        rows.append((section + f"Player{player}Controller", "1" if player == 0 else "2"))
        rows.append((section + f"Player{player}Color", str(player)))
        rows.append((section + f"Player{player}AllyGroup", "5"))
        rows.append((section + f"Player{player}Side", str(player % 2)))
    lines = ["open-annihilation-preferences 1"]
    lines.extend(quoted(key) + " " + quoted(value) for key, value in rows)
    path.write_text("\n".join(lines) + "\n")


def stage_text():
    # Full radar, so each army's blip is on the minimap and not only the local player's.
    lines = ["console +radar"]
    for player in range(PLAYERS):
        lines.append(f"place {player} ARMPW {UNIT_X0 + player * UNIT_STEP} {UNIT_Z} south")
    return "\n".join(lines) + "\n"


def run(native, game_dir, workdir, mode):
    preferences(workdir / "preferences.conf", mode)
    user = workdir / "user"
    user.mkdir(exist_ok=True)
    stage = workdir / "armies.stage"
    stage.write_text(stage_text())
    snapshot = workdir / f"{mode}.ppm"
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--skip-intro",
         "--headless-check", "--match-ticks", "1", "--seed", "1", "--resolution", "640x480",
         "--camera", f"{CAMERA_X},{CAMERA_Z}", "--stage", str(stage), "--snapshot", str(snapshot),
         "--preferences-file", str(workdir / "preferences.conf"), "--user-folder", str(user)],
        cwd=workdir,
        env=dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy"),
        timeout=TIMEOUT, check=False, capture_output=True, text=True, errors="replace")
    return result, snapshot


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
    header, _, pixels = data.partition(b"\n")
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


def unit_band(player):
    centre = ORIGIN_X + (UNIT_X0 + player * UNIT_STEP - CAMERA_X)
    left = centre - UNIT_HALF_X
    right = centre + UNIT_HALF_X
    top = ORIGIN_Y + (UNIT_Z - CAMERA_Z) - UNIT_ABOVE
    bottom = ORIGIN_Y + (UNIT_Z - CAMERA_Z) + UNIT_BELOW
    return left, right, max(ORIGIN_Y, top), min(HEIGHT - ORIGIN_Y, bottom)


def check_players(output):
    placed = [
        (name, int(player), int(x), int(z))
        for _, name, player, x, z in PLACED.findall(output)
    ]
    expected = [
        ("ARMPW", player, UNIT_X0 + player * UNIT_STEP, UNIT_Z) for player in range(PLAYERS)
    ]
    if placed != expected:
        return f"the stage placed {placed}"
    return None


def channel_distance(left, right):
    return abs(left[0] - right[0]) + abs(left[1] - right[1]) + abs(left[2] - right[2])


# Machado, Oliveira and Fernandes 2009, severity 1.0. The same matrices the
# unit test applies to linear sRGB, with each channel then held to 0..1.
MACHADO = {
    "protanopia": (
        (0.152286, 1.052583, -0.204868),
        (0.114503, 0.786281, 0.099216),
        (-0.003882, -0.048116, 1.051998),
    ),
    "deuteranopia": (
        (0.367322, 0.860646, -0.227968),
        (0.280085, 0.672501, 0.047413),
        (-0.011820, 0.042940, 0.968881),
    ),
    "tritanopia": (
        (1.255528, -0.076749, -0.178779),
        (-0.078411, 0.930809, 0.147602),
        (0.004733, 0.691367, 0.303900),
    ),
}
DICHROMACIES = ("protanopia", "deuteranopia", "tritanopia")
# Viénot, Brettel and Mollon 1999. The same matrices simulate_vienot applies
# to linear sRGB, with each channel then held to 0..1.
VIENOT = {
    "protanopia": (
        (0.11238, 0.88762, 0.0),
        (0.11238, 0.88762, 0.0),
        (0.00401, -0.00401, 1.0),
    ),
    "deuteranopia": (
        (0.29275, 0.70725, 0.0),
        (0.29275, 0.70725, 0.0),
        (-0.02234, 0.02234, 1.0),
    ),
}
# Brettel, Viénot and Mollon 1997, tritanopia. simulate_brettel takes the
# first plane when the dot product is not negative.
BRETTEL_ONE = (
    (1.01277, 0.13548, -0.14826),
    (-0.01243, 0.86812, 0.14431),
    (0.07589, 0.80500, 0.11911),
)
BRETTEL_TWO = (
    (0.93678, 0.18979, -0.12657),
    (0.06154, 0.81526, 0.12320),
    (-0.37562, 1.12767, 0.24796),
)
# An army whose unit contributes fewer pixels than this is not a measurement.
MIN_TEAM_PIXELS = 20
OUTLINE = rgb(89)


def linear_channel(channel):
    encoded = channel / 255.0
    if encoded <= 0.04045:
        return encoded / 12.92
    return ((encoded + 0.055) / 1.055) ** 2.4


def lab_f(channel):
    epsilon = 216.0 / 24389.0
    kappa = 24389.0 / 27.0
    if channel > epsilon:
        return channel ** (1.0 / 3.0)
    return (kappa * channel + 16.0) / 116.0


def lab_linear(red, green, blue):
    x = 0.4124564 * red + 0.3575761 * green + 0.1804375 * blue
    y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue
    z = 0.0193339 * red + 0.1191920 * green + 0.9503041 * blue
    return (
        116.0 * lab_f(y) - 16.0,
        500.0 * (lab_f(x / 0.95047) - lab_f(y)),
        200.0 * (lab_f(y) - lab_f(z / 1.08883)),
    )


def clamp_unit(channel):
    return 0.0 if channel < 0.0 else 1.0 if channel > 1.0 else channel


def mapped_lab(matrix, red, green, blue):
    mapped = [clamp_unit(row[0] * red + row[1] * green + row[2] * blue) for row in matrix]
    return lab_linear(*mapped)


def simulate_linear(mode, red, green, blue):
    """Machado on linear sRGB, then CIELAB. The channels are not quantised."""
    return mapped_lab(MACHADO[mode], red, green, blue)


def simulate_vienot(mode, red, green, blue):
    """Viénot on linear sRGB, then CIELAB. The channels are not quantised."""
    return mapped_lab(VIENOT[mode], red, green, blue)


def simulate_brettel(red, green, blue):
    """Brettel tritanopia on linear sRGB, then CIELAB."""
    side = 0.03901 * red - 0.02788 * green - 0.01113 * blue
    return mapped_lab(BRETTEL_ONE if side >= 0.0 else BRETTEL_TWO, red, green, blue)


def luminance_lab(red, green, blue):
    """CIE relative luminance Y, then CIELAB of that grey."""
    luminance = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue
    return lab_linear(luminance, luminance, luminance)


def ciede2000(left, right):
    """CIEDE2000, the unit test's implementation, including Sharma's pair."""
    delta_l = right[0] - left[0]
    mean_l = (left[0] + right[0]) / 2.0
    chroma_left = math.hypot(left[1], left[2])
    chroma_right = math.hypot(right[1], right[2])
    mean_chroma = (chroma_left + chroma_right) / 2.0
    mean_chroma_7 = mean_chroma ** 7.0
    boost = 0.5 * (1.0 - math.sqrt(mean_chroma_7 / (mean_chroma_7 + 25.0 ** 7.0)))
    a_left = left[1] * (1.0 + boost)
    a_right = right[1] * (1.0 + boost)
    prime_left = math.hypot(a_left, left[2])
    prime_right = math.hypot(a_right, right[2])
    mean_prime = (prime_left + prime_right) / 2.0

    def hue(a_value, b_value, chroma):
        if chroma <= 0.0:
            return 0.0
        value = math.degrees(math.atan2(b_value, a_value))
        return value + 360.0 if value < 0.0 else value

    hue_left = hue(a_left, left[2], prime_left)
    hue_right = hue(a_right, right[2], prime_right)
    if prime_left * prime_right == 0.0:
        delta_hue = 0.0
        mean_hue = hue_left + hue_right
    else:
        delta_hue = hue_right - hue_left
        if delta_hue > 180.0:
            delta_hue -= 360.0
        elif delta_hue < -180.0:
            delta_hue += 360.0
        if abs(hue_left - hue_right) <= 180.0:
            mean_hue = (hue_left + hue_right) / 2.0
        elif hue_left + hue_right < 360.0:
            mean_hue = (hue_left + hue_right + 360.0) / 2.0
        else:
            mean_hue = (hue_left + hue_right - 360.0) / 2.0
    delta_h = 2.0 * math.sqrt(prime_left * prime_right) * math.sin(math.radians(delta_hue / 2.0))
    turning = (
        1.0
        - 0.17 * math.cos(math.radians(mean_hue - 30.0))
        + 0.24 * math.cos(math.radians(2.0 * mean_hue))
        + 0.32 * math.cos(math.radians(3.0 * mean_hue + 6.0))
        - 0.20 * math.cos(math.radians(4.0 * mean_hue - 63.0))
    )
    theta = 30.0 * math.exp(-((mean_hue - 275.0) / 25.0) ** 2.0)
    mean_prime_7 = mean_prime ** 7.0
    chroma_scale = 2.0 * math.sqrt(mean_prime_7 / (mean_prime_7 + 25.0 ** 7.0))
    shift = mean_l - 50.0
    scale_l = 1.0 + (0.015 * shift * shift) / math.sqrt(20.0 + shift * shift)
    scale_c = 1.0 + 0.045 * mean_prime
    scale_h = 1.0 + 0.015 * mean_prime * turning
    rotation = -math.sin(math.radians(2.0 * theta)) * chroma_scale
    term_l = delta_l / scale_l
    term_c = (prime_right - prime_left) / scale_c
    term_h = delta_h / scale_h
    return math.sqrt(
        term_l * term_l + term_c * term_c + term_h * term_h + rotation * term_c * term_h
    )


def blip_outline(off, mode_frame, player, groups, mode):
    """Pixels of a blip other than its body stay the off picture's pixels.

    The view rectangle can cross a blip. A pixel that was slot 14 and is now
    that mode's mid colour is the mark, not a damaged outline.
    """
    body = BODY_RGB[player]
    mark_on = MARK_RGB[mode]
    for group in groups:
        xs = [x for x, _ in group]
        ys = [y for _, y in group]
        y_from = max(0, min(ys) - 1)
        y_to = min(HEIGHT, max(ys) + 2)
        x_from = max(0, min(xs) - 1)
        x_to = min(SIDE_COLUMN, max(xs) + 2)
        for y in range(y_from, y_to):
            for x in range(x_from, x_to):
                before = pixel(off, x, y)
                if before == body:
                    continue
                after = pixel(mode_frame, x, y)
                if before == MARK_OFF_RGB and after == mark_on:
                    continue
                if before != after:
                    kind = "outline" if before == OUTLINE else "pixel"
                    return (
                        f"player {player} blip {kind} {x},{y} changed "
                        f"from {before.hex()} to {after.hex()}"
                    )
    return None


def army_pixels(off, frames, player):
    """Shade pixels in the unit band that differ from off in at least one mode.

    The terrain never changes, so sand that shares an old shade drops out.
    """
    shades = SHADE_RGB[player]
    left, right, top, bottom = unit_band(player)
    chosen = []
    for y in range(top, bottom + 1):
        for x in range(left, right + 1):
            before = pixel(off, x, y)
            if before not in shades:
                continue
            if any(pixel(frame, x, y) != before for frame in frames):
                chosen.append((x, y))
    return chosen


def mean_linear(frame, chosen):
    """Mean linear colour of the chosen pixels in one picture."""
    if not chosen:
        return None
    total = [0.0, 0.0, 0.0]
    for x, y in chosen:
        drawn = pixel(frame, x, y)
        for channel in range(3):
            total[channel] += linear_channel(drawn[channel])
    count = float(len(chosen))
    return tuple(channel / count for channel in total)


def least_pair(means, eye, different_levels=False):
    best = 1.0e9
    pair = (0, 1)
    for left in range(len(means)):
        seen_left = eye(*means[left])
        for right in range(left + 1, len(means)):
            if different_levels and left % 4 == right % 4:
                continue
            distance = ciede2000(seen_left, eye(*means[right]))
            if distance < best:
                best = distance
                pair = (left, right)
    return best, pair


def eyes_of(mode):
    """Machado, and Viénot or Brettel, each mapping a linear colour to CIELAB."""

    def machado(red, green, blue, mode=mode):
        return simulate_linear(mode, red, green, blue)

    if mode == "tritanopia":
        return (("Machado", machado), ("Brettel", simulate_brettel))

    def vienot(red, green, blue, mode=mode):
        return simulate_vienot(mode, red, green, blue)

    return (("Machado", machado), ("Viénot", vienot))


def format_pair(label, distance, pair, off_distance, off_pair):
    return (
        f"{label} least pair {distance:.1f} players {pair[0] + 1} and {pair[1] + 1}, "
        f"off under that eye {off_distance:.1f} players {off_pair[0] + 1} and {off_pair[1] + 1}"
    )


def check_separation(off, frames):
    """Least pair of army means on the unit pixels, under both eyes."""
    problems = []
    pictures = [frames[mode] for mode in MODES]
    chosen = [army_pixels(off, pictures, player) for player in range(PLAYERS)]
    counts = ", ".join(str(len(pixels)) for pixels in chosen)
    print(f"team pixels: {counts}")
    for player, pixels in enumerate(chosen):
        if len(pixels) < MIN_TEAM_PIXELS:
            problems.append(f"player {player + 1} has {len(pixels)} team pixels")
    if any(not pixels for pixels in chosen):
        return problems
    off_means = [mean_linear(off, pixels) for pixels in chosen]
    for mode in DICHROMACIES:
        mode_means = [mean_linear(frames[mode], pixels) for pixels in chosen]
        for label, eye in eyes_of(mode):
            mode_least, mode_pair = least_pair(mode_means, eye)
            off_least, off_pair = least_pair(off_means, eye)
            print(f"{mode}: {format_pair(label, mode_least, mode_pair, off_least, off_pair)}")
            if mode_least < off_least:
                problems.append(
                    f"{mode} {label} least pair {mode_least:.1f} is below the off "
                    f"picture's {off_least:.1f}"
                )
    ach_means = [mean_linear(frames["achromatopsia"], pixels) for pixels in chosen]
    ach_least, ach_pair = least_pair(ach_means, luminance_lab, different_levels=True)
    print(
        f"achromatopsia: least pair under Y {ach_least:.1f} players {ach_pair[0] + 1} and "
        f"{ach_pair[1] + 1} on different levels"
    )
    return problems


def check_picture(off, mode_frame, mode):
    changed = 0
    total = WIDTH * HEIGHT
    problems = []
    anchor = ANCHOR_RGB[mode]
    for player in range(PLAYERS):
        body = BODY_RGB[player]
        wanted = anchor[player]
        blip = []
        anchor_hits = []
        left, right, top, bottom = unit_band(player)
        for y in range(HEIGHT):
            for x in range(WIDTH):
                before = pixel(off, x, y)
                after = pixel(mode_frame, x, y)
                if player == 0 and before != after:
                    changed += 1
                if x < SIDE_COLUMN and before == body and after == wanted:
                    blip.append((x, y))
                in_band = left <= x <= right and top <= y <= bottom
                # The anchor may already be a shade of this army, so a pixel
                # counts when the mode picture shows it, changed or not.
                if in_band and after == wanted:
                    anchor_hits.append((x, y))
        blip_groups = [group for group in components(blip) if len(group) >= BLIP_PIXELS]
        blip_groups = [group for group in blip_groups if max(span(group)) <= BLIP_SPAN]
        if not blip_groups:
            problems.append(
                f"player {player} minimap has {len(blip)} anchor pixels, no blip-sized group"
            )
        else:
            outline = blip_outline(off, mode_frame, player, blip_groups, mode)
            if outline:
                problems.append(outline)
        # The anchor itself is on the unit. A recoloured neighbour is not enough.
        if len(anchor_hits) < UNIT_ANCHOR_PIXELS:
            problems.append(
                f"player {player} unit has {len(anchor_hits)} anchor pixels "
                f"in {left}..{right},{top}..{bottom}"
            )
    marks = 0
    mark_on = MARK_RGB[mode]
    for y in range(HEIGHT):
        for x in range(SIDE_COLUMN):
            if pixel(off, x, y) == MARK_OFF_RGB and pixel(mode_frame, x, y) == mark_on:
                marks += 1
    if marks < 1:
        problems.append("the minimap's marks did not take the mode's mid colour")
    if changed / total > CHANGED_FRACTION:
        problems.append(f"{changed} pixels changed, {changed / total:.1%} of the picture")
    if not unchanged_block(off, mode_frame):
        problems.append("no 16 by 16 block of the picture stayed as it was")
    return problems


def unchanged_block(off, mode_frame):
    row = WIDTH * 3
    block = BLOCK * 3
    for top in range(0, HEIGHT - BLOCK + 1, 8):
        for left in range(0, WIDTH - BLOCK + 1, 8):
            same = True
            for y in range(BLOCK):
                at = (top + y) * row + left * 3
                if off[at:at + block] != mode_frame[at:at + block]:
                    same = False
                    break
            if same:
                return True
    return False


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
    with tempfile.TemporaryDirectory(prefix="A06-", dir=arguments.scratch_root) as scratch:
        workdir = Path(scratch)
        off_result, off_snapshot = run(arguments.native, arguments.game_dir, workdir, "off")
        if off_result.returncode != 0:
            fail(off_result, f"the battle with colour vision off exited {off_result.returncode}")
        placed = check_players(off_result.stdout + off_result.stderr)
        if placed:
            fail(off_result, placed)
        off = read_ppm(off_snapshot)
        if off is None:
            fail(off_result, "the off snapshot is not a 640x480 picture")
        frames = {}
        for mode in MODES:
            result, snapshot = run(arguments.native, arguments.game_dir, workdir, mode)
            if result.returncode != 0:
                fail(result, f"the {mode} battle exited {result.returncode}")
            placed = check_players(result.stdout + result.stderr)
            if placed:
                fail(result, f"{mode}: {placed}")
            frame = read_ppm(snapshot)
            if frame is None:
                fail(result, f"the {mode} snapshot is not a 640x480 picture")
            frames[mode] = frame
            snapshot.unlink()
        for mode, frame in frames.items():
            problems = check_picture(off, frame, mode)
            if problems:
                print("\n".join(problems))
                fail(
                    off_result,
                    f"{mode} did not show each army's anchor on its unit and its minimap dot"
                )
        separated = check_separation(off, frames)
        if separated:
            print("\n".join(separated))
            fail(
                off_result,
                "a dichromacy's armies are closer than with the mode off, or a unit "
                "has fewer than 20 team pixels"
            )
        off_snapshot.unlink()
    print(
        "colour vision check: ten players in each mode show that mode's anchor "
        "on the unit and the minimap dot, the blip outline stays, and each "
        "dichromacy's armies stay at least as far apart as with the mode off "
        "under both eyes"
    )


if __name__ == "__main__":
    main()
