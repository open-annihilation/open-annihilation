#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that the Open Annihilation settings leave the game as it plays without them.

A seeded headless skirmish, its local army marching, is played through the
match clock and drawn at 30 frames a second (--frame-rate), each run from a
preferences file the check writes:

- With no preferences file, and with one holding every setting's key at its
  default value, it writes the same trace stream, reaches the same world
  digest and draws the same last frame, byte for byte. So it does with HUD
  scaling Off alone: at the run's 640x480 the side column and bars are at
  the original game's size either way; and with Smooth motion Off alone: at
  the run's 30 frames a second every frame shows its tick whole either way.
- With every setting that only changes the look or the input away from its
  default (wheel zoom off, the zoom's limits at Whole map and None, Escape
  opens the game menu, Select groups without Alt, the lowest maximum frame
  rate, the performance statistics, hardware
  acceleration at Full and vertical sync, which a headless run has no
  renderer for, Smooth motion off, Allow Full 3D Mode off and Unlocked
  Camera on, the eight effect sliders at 0%, the explosions' flashes off,
  the game's own unit shadows (New unit shadows off),
  units zoomed out as dots past half scale, the window's frame always
  shown, and the side column and bars at the original game's size), it
  writes the same trace stream and reaches the same world digest.
- At each level of enhanced anti-aliasing it writes the same trace stream
  and reaches the same world digest, and its last frame differs: units are
  drawn finer.
- With Pathfinding cycles at 2x, two runs write the same trace stream, and it
  differs from the default's; with a unit limit of 500, two runs write the
  same trace stream.

The director render (--check-director-render), whose frames and sound are
pinned, keeps its original frame hashes with the four local lighting and heat
effects off, also with enhanced anti-aliasing at 16x. With those effects on
it keeps the same pinned sound; its internal checks compare the drawn world
with the undrawn replay.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

TICKS = 150
COMBAT_UNITS = 16
SEED = 1234567
FRAME_RATE = 30

# A preferences file's first line.
PREFERENCES_HEADER = "open-annihilation-preferences 1"
# 3.1c's own SwitchAlt, which Select groups without Alt sets.
SWITCH_ALT_KEY = "Total Annihilation|SwitchAlt"
# Every Open Annihilation setting's key with its default value, as a
# preferences file named on the command line gives it.
DEFAULTS = {
    "open-annihilation.path-search-nodes": "1333",
    "open-annihilation.wheel-zoom": "1",
    "open-annihilation.max-zoom-out": "automatic",
    "open-annihilation.max-zoom-in": "4",
    "open-annihilation.view-past-map-edge": "50",
    "open-annihilation.escape-opens-menu": "0",
    "open-annihilation.unit-limit": "250",
    "open-annihilation.max-fps": "120",
    "open-annihilation.anti-aliasing": "1",
    "open-annihilation.frame-stats": "0",
    "open-annihilation.screen-size": "desktop",
    "open-annihilation.hardware-acceleration": "off",
    "open-annihilation.vertical-sync": "0",
    "open-annihilation.smooth-motion": "1",
    "open-annihilation.allow-full-3d": "1",
    "open-annihilation.unlocked-camera": "0",
    "open-annihilation.weapon-lighting.intensity": "0",
    "open-annihilation.nanolathe-lighting.intensity": "0",
    "open-annihilation.wreck-glow.intensity": "0",
    "open-annihilation.heat-distortion.intensity": "0",
    "open-annihilation.water-ripples.intensity": "60",
    "open-annihilation.coastal-waves.intensity": "30",
    "open-annihilation.underwater-shimmer.intensity": "100",
    "open-annihilation.naval-reflections.intensity": "100",
    "open-annihilation.explosion-flash": "full",
    "open-annihilation.fog-of-war": "smooth",
    "open-annihilation.new-unit-shadows": "1",
    "open-annihilation.zoomed-out-units": "rendered",
    "open-annihilation.zoomed-out-after": "1/6",
    "open-annihilation.window-frame": "hidden-in-play",
    "open-annihilation.hud-scaling": "1",
    "open-annihilation.modern-fonts": "0",
    "open-annihilation.text-outline": "1",
    "open-annihilation.text-shadow": "1",
    "open-annihilation.text-background": "0",
    "open-annihilation.developer-mode": "0",
}
# The settings that change only the look or the input, away from their defaults.
PRESENTATION = {
    "open-annihilation.wheel-zoom": "0",
    "open-annihilation.max-zoom-out": "whole-map",
    "open-annihilation.max-zoom-in": "1",
    "open-annihilation.view-past-map-edge": "off",
    "open-annihilation.escape-opens-menu": "1",
    "open-annihilation.max-fps": "40",
    "open-annihilation.frame-stats": "1",
    "open-annihilation.hardware-acceleration": "full",
    "open-annihilation.vertical-sync": "1",
    "open-annihilation.smooth-motion": "0",
    "open-annihilation.allow-full-3d": "0",
    "open-annihilation.unlocked-camera": "1",
    "open-annihilation.weapon-lighting.intensity": "100",
    "open-annihilation.nanolathe-lighting.intensity": "100",
    "open-annihilation.wreck-glow.intensity": "100",
    "open-annihilation.heat-distortion.intensity": "100",
    "open-annihilation.water-ripples.intensity": "0",
    "open-annihilation.coastal-waves.intensity": "0",
    "open-annihilation.underwater-shimmer.intensity": "0",
    "open-annihilation.naval-reflections.intensity": "0",
    "open-annihilation.explosion-flash": "off",
    "open-annihilation.fog-of-war": "classic",
    "open-annihilation.new-unit-shadows": "0",
    "open-annihilation.zoomed-out-units": "dots",
    "open-annihilation.zoomed-out-after": "1/2",
    "open-annihilation.window-frame": "always-shown",
    "open-annihilation.hud-scaling": "0",
    "open-annihilation.modern-fonts": "1",
    "open-annihilation.text-outline": "0",
    "open-annihilation.text-shadow": "0",
    "open-annihilation.text-background": "1",
    SWITCH_ALT_KEY: "1",
}
# The director frame reference draws the defaults: a named file's acceleration
# is Off, so local lighting and heat are off too.
DIRECTOR_REFERENCE = dict(DEFAULTS)
HUD_SCALING_KEY = "open-annihilation.hud-scaling"
SMOOTH_MOTION_KEY = "open-annihilation.smooth-motion"
ANTI_ALIASING_KEY = "open-annihilation.anti-aliasing"
ANTI_ALIASING_LEVELS = (2, 3, 4, 8, 16)
PATH_SEARCH_KEY = "open-annihilation.path-search-nodes"
DOUBLED_PATH_NODES = 2666
UNIT_LIMIT_KEY = "open-annihilation.unit-limit"
RAISED_UNIT_LIMIT = 500

RUN_LINE = re.compile(r"frame run: .* world digest ([0-9a-f]{16})")


def write_preferences(path, values):
    """Writes a preferences file holding the given keys."""
    lines = [PREFERENCES_HEADER]
    lines += [f'"{key}" "{value}"' for key, value in sorted(values.items())]
    path.write_text("\n".join(lines) + "\n")


def skirmish(native, game_dir, workdir, name, values):
    """Plays the drawn skirmish from a preferences file, or none when values is None.

    Returns the world digest, the trace stream's bytes and the last frame's bytes.
    """
    profile = workdir / f"{name}.conf"
    if values is not None:
        write_preferences(profile, values)
    trace = workdir / f"{name}.trace"
    frame = workdir / f"{name}.ppm"
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--headless-check", "--preferences-file", str(profile),
         "--match-ticks", str(TICKS), "--combat", str(COMBAT_UNITS), "--march",
         "--seed", str(SEED), "--trace-digest", str(trace), "--frame-rate", str(FRAME_RATE),
         "--snapshot", str(frame)],
        cwd=workdir, timeout=600, check=False, capture_output=True, text=True,
    )
    match = RUN_LINE.search(result.stdout)
    if result.returncode != 0 or match is None:
        print(result.stdout + result.stderr, end="")
        raise SystemExit(f"skirmish {name} exited with {result.returncode}")
    return match.group(1), trace.read_bytes(), frame.read_bytes()


def director_render(native, game_dir, workdir, name, values, pinned, compare_frames=True):
    """Checks the director's pinned sound, and frames for the original visual reference."""
    directory = workdir / name
    directory.mkdir()
    profile = directory / "director-render.conf"
    write_preferences(profile, values)
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    result = subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--mute", "--check-director-render",
         "--preferences-file", str(profile)],
        cwd=directory, env=environment, timeout=600, check=False, capture_output=True, text=True,
    )
    expected = pinned if compare_frames else "sound " + pinned.split(" sound ", 1)[1]
    if result.returncode != 0 or expected not in result.stdout:
        print(result.stdout + result.stderr, end="")
        checked = "frames and sound" if compare_frames else "sound"
        raise SystemExit(f"the director render {name} did not give its pinned {checked}")


def short(data):
    """Returns a short hash of some bytes, for the messages."""
    return hashlib.sha256(data).hexdigest()[:16]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path, required=True)
    parser.add_argument("--director-hashes", required=True,
                        help="the line the director render prints with its pinned hashes")
    args = parser.parse_args()
    native = args.native.resolve()
    game_dir = args.game_dir.resolve()
    args.scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="native-engine-settings-",
                                     dir=args.scratch_root.resolve()) as temporary:
        workdir = Path(temporary)
        digest, trace, frame = skirmish(native, game_dir, workdir, "none", None)
        defaults = skirmish(native, game_dir, workdir, "defaults", DEFAULTS)
        if defaults != (digest, trace, frame):
            raise SystemExit(
                f"every key at its default moved the game: digest {defaults[0]} trace "
                f"{short(defaults[1])} frame {short(defaults[2])}, not {digest} {short(trace)} "
                f"{short(frame)}")
        unscaled = skirmish(native, game_dir, workdir, "unscaled", {**DEFAULTS, HUD_SCALING_KEY: "0"})
        if unscaled != (digest, trace, frame):
            raise SystemExit(
                f"HUD scaling Off at 640x480 moved the game or its picture: digest {unscaled[0]} "
                f"frame {short(unscaled[2])}, not {digest} {short(frame)}")
        ticked = skirmish(native, game_dir, workdir, "ticked", {**DEFAULTS, SMOOTH_MOTION_KEY: "0"})
        if ticked != (digest, trace, frame):
            raise SystemExit(
                f"Smooth motion Off at 30 frames a second moved the game or its picture: digest "
                f"{ticked[0]} trace {short(ticked[1])} frame {short(ticked[2])}, not {digest} "
                f"{short(trace)} {short(frame)}")
        shown = skirmish(native, game_dir, workdir, "presentation", {**DEFAULTS, **PRESENTATION})
        if shown[:2] != (digest, trace):
            raise SystemExit(f"the look and input settings moved the world: digest {shown[0]}, "
                             f"not {digest}")
        for level in ANTI_ALIASING_LEVELS:
            smoothed = skirmish(native, game_dir, workdir, f"aa{level}",
                                {**DEFAULTS, ANTI_ALIASING_KEY: str(level)})
            if smoothed[:2] != (digest, trace):
                raise SystemExit(f"enhanced anti-aliasing at {level}x moved the world: digest "
                                 f"{smoothed[0]}, not {digest}")
            if smoothed[2] == frame:
                raise SystemExit(f"enhanced anti-aliasing at {level}x drew the same last frame")
        paths = [skirmish(native, game_dir, workdir, f"path{run}",
                          {**DEFAULTS, PATH_SEARCH_KEY: str(DOUBLED_PATH_NODES)})
                 for run in (1, 2)]
        if paths[0][:2] != paths[1][:2]:
            raise SystemExit("two runs with Pathfinding cycles at 2x wrote different streams")
        if paths[0][1] == trace:
            raise SystemExit("Pathfinding cycles at 2x left the marching army's paths as they were")
        limits = [skirmish(native, game_dir, workdir, f"limit{run}",
                           {**DEFAULTS, UNIT_LIMIT_KEY: str(RAISED_UNIT_LIMIT)})
                  for run in (1, 2)]
        if limits[0][:2] != limits[1][:2]:
            raise SystemExit(f"two runs with a unit limit of {RAISED_UNIT_LIMIT} wrote "
                             "different streams")
        director_render(native, game_dir, workdir, "director-reference", DIRECTOR_REFERENCE,
                        args.director_hashes)
        director_render(native, game_dir, workdir, "director-aa16",
                        {**DIRECTOR_REFERENCE, ANTI_ALIASING_KEY: "16"}, args.director_hashes)
        director_render(native, game_dir, workdir, "director-effects", DEFAULTS,
                        args.director_hashes, compare_frames=False)
        print(f"engine settings determinism: digest {digest} with no preferences, every default, "
              f"HUD scaling Off, Smooth motion Off, the look and input settings and "
              f"anti-aliasing at "
              f"{', '.join(f'{level}x' for level in ANTI_ALIASING_LEVELS)}; Pathfinding cycles "
              f"at 2x {paths[0][0]}, unit limit {RAISED_UNIT_LIMIT} {limits[0][0]}; the director "
              f"render's original frames and sound with local effects Off and at 16x, "
              f"and the same sound with local effects On")


if __name__ == "__main__":
    main()
