#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""Check that --bench plays the bench's battle for a few seconds and prints well-formed lines.

The game starts on the dummy video and sound drivers with the bench's own
preferences (tools/bench/preferences.conf) and a short view file that plays
the bench's battle stage (tools/bench/battle.stage) on Lava Highground: a
warm-up of WARMUP_TICKS ticks and a measured span of MEASURE_SECONDS
seconds, the camera turned. The run must print, in order, INITIAL, WARMUP,
MEASURING, a SEC line for each second, SHOT, RESULT, the five MEASURE
spreads with the tick's mean, a STAGE line for every stage timer under its
name, COUNTS, GROUND, ATLAS, the SLOW lines of the slowest 1% of frames, a
FRAME and SIM_FRAME line for every frame (the view asks for the frame log) and DONE;
every spread must be in order (least, median, 95th and 99th percentiles,
slowest 1%, most), the frame rates must agree with the frames and seconds,
the slowest 1% must be as many frames as the span's 1% and as slow as its
1% low, the frame log's frames must be the span's, and the stage must have
placed its armies and queued its factories. tools/bench/summarise.py must
read the run. A second run with the stage timers off prints no STAGE or
SLOW line, and a view file with a key the bench does not know stops the
bench with a FAILED line naming the file's line, and the game with status 1.
Short paused runs also check default and explicit Enhanced presets without
making a running-gameplay or frame-rate claim.
"""
import argparse
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import native_offline

# A command that starts the executable under test, such as the compatibility
# layer a cross build runs its tests through (tools/build_windows.sh --run-tests
# sets it); empty runs the executable directly.
RUNNER = shlex.split(os.environ.get("OA_TEST_RUNNER", ""))

BENCH = Path(__file__).resolve().parent / "bench"
RUN_TIMEOUT = 300
WARMUP_TICKS = 30
MEASURE_SECONDS = 3
CURRENT_RULES_VERSION = 4
SUPPORTED_RULES_VERSIONS = frozenset((*range(9), 10, 11, 13))
VIEW = """label check
stage {stage}
menu_seconds 0
warmup_ticks {warmup}
measure_seconds {seconds}
shot_at 1
centre_x 1980
centre_z 2080
yaw 20
pitch -53
zoom 1
stage_timers {timers}
frame_log {timers}
"""
# The stage timers' names, in the order the STAGE lines give them: the names
# stay the same from one release to the next, so that runs compare.
STAGES = (
    "frame(render_match_surface)", "live_total", "scene_build", "  survey+plan", "  terrain",
    "  ground_sprites", "  unit_shadows", "  foundations", "  models", "  projectiles",
    "  sprites", "  particles", "  water+rest", "gpu_total", "  validate", "  upload_copy",
    "  submit+wait", "  download_copy", "rgba_to_rgb", "picking", "effects",
    "overlays(draw_live_overlays)", "ds_setup", "ds_hud(compose_match_hud)",
    "ds_plan(draw list)", "ds_post(fog,bars)", "ds_live(render_live_match_surface)",
    "ds_after(megamap,hud text)", "ov_selection", "ov_unit_overlays", "ov_on_screen_units",
    "cursor_pick", "  effect_visibility", "overlay_on_card",
)
MEASURES = ("frame", "work", "sim(per frame)", "draw", "present")
NUMBER = r"(\d+(?:\.\d+)?)"
RESULT = re.compile(
    rf"^bench: RESULT label=check seconds {NUMBER} frames (\d+) fps {NUMBER} low1_fps {NUMBER} "
    rf"ticks (\d+) \({NUMBER}/s\) game speed (\d+) of (\d+)$")
MEASURE = re.compile(
    rf"^bench: MEASURE (\S+(?: \S+)?)\s+min\s+{NUMBER} avg\s+{NUMBER} p50\s+{NUMBER} "
    rf"p95\s+{NUMBER} p99\s+{NUMBER} low1\s+{NUMBER} max\s+{NUMBER}$")
TICK = re.compile(rf"^bench: MEASURE tick\(per tick\)\s+avg\s+{NUMBER}$")
STAGE = re.compile(rf"^bench: STAGE (.+?)\s+{NUMBER}$")
SEC = re.compile(
    rf"^bench: SEC (\d+) frames (\d+) frame {NUMBER} sim {NUMBER} draw {NUMBER} "
    rf"units (\d+)\+(\d+) vertices (\d+) tick (\d+)$")
COUNTS = re.compile(r"^bench: COUNTS units (\d+) \(player0 (\d+), others (\d+)\) drawn (\d+) ")
AWAY = re.compile(r"^bench: AWAY label=check frames (\d+) of (\d+): the window was away, .+$")
GROUND = re.compile(r"^bench: GROUND revisions (\d+) in (\d+) frames$")
ATLAS = re.compile(r"^bench: ATLAS terrain_packs [\d.]+ a frame, atlas_resets (\d+) in (\d+) frames$")
SLOW = re.compile(
    rf"^bench: SLOW frames (\d+) of (\d+), the slowest 1%: frame avg {NUMBER} ground_revisions "
    rf"(\d+) terrain_packs {NUMBER} a frame atlas_resets (\d+) atlas_rows (\d+) a frame "
    rf"full_atlas_uploads (\d+)$")
SLOW_STAGE = re.compile(rf"^bench: SLOW STAGE (.+?)\s+{NUMBER}$")
SLOW_ATLAS = re.compile(
    rf"^bench: SLOW ATLAS packed by (.+?)\s+{NUMBER} materials\s+(\d+) texels a frame$")
FRAME = re.compile(
    rf"^bench: FRAME (\d+) frame {NUMBER} work {NUMBER} draw {NUMBER} ground ([01]) "
    rf"terrain_packs (\d+) atlas_resets (\d+) atlas_rows (\d+) materials (\d+) "
    rf"stages((?: {NUMBER}){{{len(STAGES)}}})$")
SIM_FRAME = re.compile(
    rf"^bench: SIM_FRAME (?P<frame>\d+) sim_ms (?P<sim>{NUMBER}) "
    r"tick_before (?P<before>\d+) tick_after (?P<after>\d+) ticks (?P<ticks>\d+) "
    rf"steps (?P<steps>\d+) max_step_ms (?P<maximum>{NUMBER}) "
    r"max_step_tick (?P<slowest>\d+)$")
UINT32_MASK = (1 << 32) - 1
UNIT_DETAIL_TAGS = frozenset(("UNIT_GEOMETRY", "UNIT_DETAIL_FRAME"))
UNSIGNED = r"(?:0|[1-9]\d*)"
SIGNED = r"(?:0|-?[1-9]\d*)"
UNIT_GEOMETRY = re.compile(
    rf"^bench: UNIT_GEOMETRY type (?P<type>{UNSIGNED}) present (?P<present>{UNSIGNED}) "
    rf"owned (?P<owned>{UNSIGNED}) rest_mesh_x (?P<x>\d+\.\d{{4}}) "
    r"rest_mesh_y (?P<y>\d+\.\d{4}) rest_mesh_z (?P<z>\d+\.\d{4}) "
    rf"footprint_x (?P<footprint_x>{SIGNED}) footprint_z (?P<footprint_z>{SIGNED}) name=(?P<name>\S+)$")
UNIT_DETAIL_FRAME = re.compile(
    rf"^bench: UNIT_DETAIL_FRAME (?P<frame>{UNSIGNED}) automatic (?P<automatic>[01]) "
    r"average_pixels (?P<average>\d+\.\d{4}) dot_pixels (?P<dot>\d+\.\d) "
    rf"model_ratio (?P<ratio>\d+\.\d{{2}}) reference_units (?P<units>{UNSIGNED}) dots (?P<dots>[01])$")
BODY_LOD_FRAME = re.compile(
    rf"^bench: BODY_LOD_FRAME (?P<frame>{UNSIGNED}) full_mesh_models (?P<full>{UNSIGNED}) "
    rf"body_lod_models (?P<body>{UNSIGNED}) distance_markers (?P<distance>{UNSIGNED}) "
    rf"budget_markers (?P<budget>{UNSIGNED}) full_mesh_vertices (?P<full_vertices>{UNSIGNED}) "
    rf"body_lod_vertices (?P<body_vertices>{UNSIGNED}) shadow_vertices (?P<shadow>{UNSIGNED}) "
    rf"deferred_geometry (?P<deferred>{UNSIGNED})$")
SCENE_WORK_FRAME = re.compile(
    rf"^bench: SCENE_WORK_FRAME (?P<frame>{UNSIGNED}) candidate_bands (?P<candidate_bands>{UNSIGNED}) "
    rf"candidate_slots (?P<candidate_slots>{UNSIGNED}) accepted_bands (?P<accepted_bands>{UNSIGNED}) "
    rf"accepted_slots (?P<accepted_slots>{UNSIGNED}) fallback_frames (?P<fallback_frames>{UNSIGNED})$")
LEGACY_SHADOW_INDEX_UINT64_FIELDS = (
    "logical_indices", "dynamic_indices", "cached_indices", "dynamic_uploaded_indices",
    "template_uploaded_indices", "cache_bytes",
)
# Mapping uploads count API payload words independently of drawn index corners.
SHADOW_INDEX_UINT64_FIELDS = (*LEGACY_SHADOW_INDEX_UINT64_FIELDS, "mapping_uploaded_words")
SHADOW_INDEX_UINT32_FIELDS = (
    "requested_runs", "cached_runs", "cache_hits", "cache_misses", "admitted_patterns",
    "fallback_runs", "cached_patterns",
)
FRAME_WORK_UNSIGNED = r"(?:0|[1-9][0-9]*)"
LEGACY_SHADOW_INDEX_FRAME = re.compile(
    rf"^bench: SHADOW_INDEX_FRAME (?P<frame>{FRAME_WORK_UNSIGNED})" +
    "".join(rf" {name} (?P<{name}>{FRAME_WORK_UNSIGNED})"
            for name in (*LEGACY_SHADOW_INDEX_UINT64_FIELDS, *SHADOW_INDEX_UINT32_FIELDS)) + "$")
SHADOW_INDEX_FRAME = re.compile(
    LEGACY_SHADOW_INDEX_FRAME.pattern[:-1] +
    rf" mapping_uploaded_words (?P<mapping_uploaded_words>{FRAME_WORK_UNSIGNED})$")
SHADOW_CACHE_BYTES = 8 << 20
SHADOW_UPLOAD_BYTES = 1 << 20
SHADOW_CACHE_PATTERNS = 64
SHADOW_FRAME_ADMISSIONS = 2
SHADOW_INDEX_BYTES = 4
LEGACY_PLAN_WORK_FIELDS = (
    "unit_plans", "copy_plans", "linked_plans", "omitted_plans", "original_updates_dirty",
    "copy_updates_dirty", "transform_cache_hits", "root_prefix_hits", "transform_rebuilds",
    "metadata_image_builds", "pixel_image_builds",
)
ROOT_PREFIX_WORK_FIELDS = (
    "root_prefix_recipe_publishes", "root_prefix_scene_instances",
    "root_prefix_gpu_instances", "root_prefix_gpu_surface_indices",
)
PLAN_WORK_FIELDS = (*LEGACY_PLAN_WORK_FIELDS, *ROOT_PREFIX_WORK_FIELDS)
PLAN_WORK_PHASES = (
    "pose_capture", "list_prepare", "unit_sites", "battlefield_sort", "run_prepare", "run_wait",
    "worker_plan", "copy_place", "original_note", "original_update", "copy_note", "copy_update",
    "omission_proof", "model_plan_metadata", "model_plan_pixels",
)
PLAN_PHASE_FIELDS = ("calls", "samples", "wall_ns", "cpu_ns", "cpu_samples")
PLAN_WORK_FRAME = re.compile(
    rf"^bench: PLAN_WORK_FRAME (?P<frame>{FRAME_WORK_UNSIGNED}) "
    rf"source_frame (?P<source_frame>{FRAME_WORK_UNSIGNED}) "
    r"completed (?P<completed>[01]) sampling_stride 64" +
    "".join(rf" {name} (?P<{name}>{FRAME_WORK_UNSIGNED})" for name in LEGACY_PLAN_WORK_FIELDS) +
    "(?:" + "".join(rf" {name} (?P<{name}>{FRAME_WORK_UNSIGNED})"
                    for name in ROOT_PREFIX_WORK_FIELDS) + ")? phases" +
    "".join(" " + phase + "".join(rf" {name} (?P<{phase}_{name}>{FRAME_WORK_UNSIGNED})"
                                for name in PLAN_PHASE_FIELDS)
            for phase in PLAN_WORK_PHASES) + "$")
FRAME_WORK_TAGS = frozenset(("SHADOW_INDEX_FRAME", "PLAN_WORK_FRAME"))
SCENE_WORK_MAX_BANDS = 4
UINT64_MASK = (1 << 64) - 1
UNIT_DOT_PIXELS = 5.0
UNIT_MODEL_TO_DOT_RATIO = 1.2
FRAME_RESULT = re.compile(RESULT.pattern.replace("label=check", r"label=\S+"))
# Rest-mesh coordinates are signed 16.16; their difference can round to this size.
UNIT_MAX_MESH_EXTENT = 65536.0
# A sum and its maximum are independently rounded to three decimal places.
SIM_ROUNDING_MS = 0.0011
# The frame mean is rounded to two places, the SIM_FRAME samples to three.
SIM_MEAN_ROUNDING_MS = 0.0056
# Renderer records are separate from the stable FRAME fields and stage order.
MATERIAL_KINDS = ("surface", "scenery", "effect", "light", "shadow")
RENDERER_TAGS = frozenset(("GROUND_INDEX", "POSE", "MATERIAL", "GROUND_INDEX_FRAME",
                          "POSE_FRAME", "MATERIAL_FRAME", "GROUND_INDEX_BUILD_FRAME",
                          "GROUND_INDEX_BUILD_DROPPED_FRAME"))
GROUND_INDEX = re.compile(rf"^bench: GROUND_INDEX builds (\d+) hits (\d+) triangles (\d+) wall_ms {NUMBER}$")
GROUND_INDEX_FRAME = re.compile(rf"^bench: GROUND_INDEX_FRAME (\d+) builds (\d+) hits (\d+) triangles (\d+) wall_ms {NUMBER}$")
POSE = re.compile(r"^bench: POSE units (\d+) pieces (\d+) vm_triplets (\d+)$")
POSE_FRAME = re.compile(r"^bench: POSE_FRAME (\d+) units (\d+) pieces (\d+) vm_triplets (\d+)$")
MATERIAL = re.compile(r"^bench: MATERIAL (\S+) insertions (\d+) texels (\d+) row_texels (\d+)$")
MATERIAL_FRAME = re.compile(r"^bench: MATERIAL_FRAME (\d+) (\S+) insertions (\d+) texels (\d+) row_texels (\d+)$")
HEX_AXES = r"[0-9a-f]{16}(?:,[0-9a-f]{16}){3}"
HEX_CONTROL = r"[0-9a-f]{1,16}(?:,[0-9a-f]{1,16}){2}"
INDEX_BUILD_CAPACITY = 8
# Three phase clocks and their wall are independently rounded to three places.
INDEX_BUILD_ROUNDING_MS = 0.0021
INDEX_BUILD = re.compile(
    r"^bench: GROUND_INDEX_BUILD_FRAME (?P<frame>\d+) ordinal (?P<ordinal>\d+) "
    r"previous_attempts (?P<previous_attempts>\d+) was_valid (?P<was_valid>[01]) "
    r"completed (?P<completed>[01]) reason_mask (?P<reasons>\d+) "
    r"old_revision (?P<old_revision>\d+) revision (?P<revision>\d+) "
    r"old_vertices (?P<old_vertices>\d+) vertices (?P<vertices>\d+) "
    r"old_card (?P<old_card>[01]) card (?P<card>[01])(?: parallel (?P<parallel>[01]))? "
    rf"old_axes (?P<old_axes>{HEX_AXES}) axes (?P<axes>{HEX_AXES}) "
    rf"old_fp (?P<old_fp>{HEX_CONTROL}) fp (?P<fp>{HEX_CONTROL}) "
    r"bounds_ms (?P<bounds>\d+(?:\.\d+)?) world_ms (?P<world>\d+(?:\.\d+)?) "
    r"card_ms (?P<card_ms>\d+(?:\.\d+)?) wall_ms (?P<wall>\d+(?:\.\d+)?)$")
INDEX_DROPPED = re.compile(r"^bench: GROUND_INDEX_BUILD_DROPPED_FRAME (\d+) count (\d+)$")

QUEUED = re.compile(r"^stage: unit \d+ queues (\d+) (\S+)$", re.M)
# The battle's armies, factories, builders and economies, placed at its
# start: at least this many live units once it has begun.
LEAST_UNITS = 400
INITIAL = re.compile(
    r"^bench: INITIAL enhanced_mode ([01]) rules_version (\d+) requested_rules_version (\d+)$")


class BenchFailure(Exception):
    """A bench run that did not print what it should."""


def _offline_preferences(path):
    """Leaves both online checks off so the run does not fetch a catalogue."""
    target = Path(path)
    target.parent.mkdir(parents=True, exist_ok=True)
    text = (target.read_text(encoding="utf-8") if target.is_file()
            else "open-annihilation-preferences 1\n")
    if "open-annihilation.update-check" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.update-check" "never"\n'
    if "open-annihilation.content-updates" not in text:
        if not text.endswith("\n"):
            text += "\n"
        text += '"open-annihilation.content-updates" "never"\n'
    target.write_text(text, encoding="utf-8")


def run_bench(native, game_dir, workdir, name, view_text, enhanced=False):
    """Runs the game with --bench on a view file and returns its result."""
    folder = workdir / name
    (folder / "data").mkdir(parents=True)
    preferences = folder / "preferences.conf"
    shutil.copyfile(BENCH / "preferences.conf", preferences)
    view = folder / f"{name}.bench"
    view.write_text(view_text, encoding="utf-8")
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy", SDL_AUDIO_DRIVER="dummy")
    mode = ["--enhanced-mode"] if enhanced else []
    _offline_preferences(preferences)
    return subprocess.run(
        [*RUNNER, str(native), "--game-dir", str(game_dir), "--skip-intro", "--mute",
         "--bench", str(view), "--preferences-file", str(preferences), "--data-dir",
         str(folder / "data"), "--resolution", "640x480", *mode],
        cwd=folder, env=environment, timeout=RUN_TIMEOUT, check=False, capture_output=True,
        text=True, errors="replace")


def fail(result, message):
    print(result.stdout + result.stderr, end="")
    raise BenchFailure(message)


def check_rules_rejection(result, expected):
    """Checks invalid rules stop at their error before the battle begins."""
    lines = result.stdout.splitlines()
    if result.returncode != 1 or expected not in lines:
        fail(result, "an unsupported rules version was not refused at its line")
    started = ("Offline match world prepared", "stage: ", "bench: INITIAL ",
               "bench: WARMUP ", "bench: MEASURING ", "bench: SEC ",
               "bench: STATIC ", "bench: FRAME ", "bench: RESULT ", "bench: DONE ")
    if any(line.startswith(started) for line in lines + result.stderr.splitlines()):
        fail(result, "unsupported rules started a battle before being refused")


def ordered(values):
    return all(left <= right for left, right in zip(values, values[1:]))


def check_initial(result, rules_version=0, requested_rules_version=0):
    """Checks the one INITIAL receipt against the run's actual and requested presets."""
    initial = [line for line in result.stdout.splitlines() if line.startswith("bench: INITIAL")]
    if len(initial) != 1:
        fail(result, f"the bench printed {len(initial)} INITIAL receipts, not one")
    match = INITIAL.fullmatch(initial[0])
    if not match:
        fail(result, f"the INITIAL line does not read: {initial[0]}")
    enhanced, actual, requested = (int(value) for value in match.groups())
    if actual not in SUPPORTED_RULES_VERSIONS or requested not in SUPPORTED_RULES_VERSIONS or \
            enhanced != int(actual != 0) or (requested != 0 and requested != actual):
        fail(result, f"the INITIAL rules are inconsistent: {initial[0]}")
    if actual != rules_version or requested != requested_rules_version:
        fail(result, f"the INITIAL rules are {actual}/{requested}, "
             f"not {rules_version}/{requested_rules_version}")


def check_unit_detail_records(result, lines):
    """Validate optional mesh metadata and paired detail rows before removing only those rows."""
    tags = [line.split()[1] for line in lines]
    if any(tag.startswith("UNIT_") and tag not in UNIT_DETAIL_TAGS for tag in tags):
        fail(result, "unit detail records: unknown unit metadata tag")
    geometry = [index for index, tag in enumerate(tags) if tag == "UNIT_GEOMETRY"]
    detail = [index for index, tag in enumerate(tags) if tag == "UNIT_DETAIL_FRAME"]
    if not geometry and not detail:
        return lines

    def require(condition, message):
        if not condition:
            fail(result, "unit detail records: " + message)

    positions = [index for index, tag in enumerate(tags) if tag == "FRAME"]
    results = [FRAME_RESULT.fullmatch(line) for line in lines if line.startswith("bench: RESULT ")]
    require(len(results) == 1 and results[0] is not None, "missing or malformed RESULT")
    frames = int(results[0].group(2))
    require(frames > 0 and len(positions) == frames, "metadata needs the complete FRAME span")
    for frame, position in enumerate(positions):
        match = FRAME.fullmatch(lines[position])
        require(match is not None and int(match.group(1)) == frame,
                "FRAME indices are not complete and ordered")
    if geometry:
        grounds = [index for index, tag in enumerate(tags) if tag == "GROUND"]
        atlases = [index for index, tag in enumerate(tags) if tag == "ATLAS"]
        require(len(grounds) == 1 and atlases and
                geometry[0] == grounds[0] + 1 and geometry[-1] < atlases[0] < positions[0],
                "mesh metadata must follow GROUND and precede ATLAS and FRAME")
        require(geometry == list(range(geometry[0], geometry[-1] + 1)),
                "mesh metadata is not contiguous")
        previous_type = -1
        present_total = 0
        for index in geometry:
            match = UNIT_GEOMETRY.fullmatch(lines[index])
            require(match is not None, "a UNIT_GEOMETRY line does not read")
            row = match.groupdict()
            unit_type, present, owned = (int(row[key]) for key in ("type", "present", "owned"))
            require(previous_type < unit_type <= (1 << 16) - 1,
                    "mesh types are repeated, unordered or exceed uint16")
            require(0 <= owned <= present <= UINT32_MASK and present > 0,
                    "invalid mesh population counts")
            require(all(-(1 << 15) <= int(row[key]) < 1 << 15
                        for key in ("footprint_x", "footprint_z")),
                    "mesh footprint exceeds int16")
            dimensions = [float(row[key]) for key in ("x", "y", "z")]
            require(all(math.isfinite(value) and 0 <= value <= UNIT_MAX_MESH_EXTENT
                        for value in dimensions),
                    "mesh extent is not finite or exceeds the rest-coordinate range")
            previous_type = unit_type
            present_total += present
        counts = [COUNTS.match(line) for line in lines if line.startswith("bench: COUNTS ")]
        require(len(counts) == 1 and counts[0] is not None and
                present_total <= int(counts[0].group(1)) <= UINT32_MASK,
                "mesh population exceeds the final unit census")
    if detail:
        require(len(detail) == frames, "each FRAME needs exactly one UNIT_DETAIL_FRAME")
        for frame, position in enumerate(positions):
            require(position + 2 < len(lines), "missing detail row after SIM_FRAME")
            simulation = SIM_FRAME.fullmatch(lines[position + 1])
            require(simulation is not None and int(simulation.group("frame")) == frame,
                    "FRAME must immediately precede its SIM_FRAME")
            require(detail[frame] == position + 2, "UNIT_DETAIL_FRAME must immediately follow SIM_FRAME")
            match = UNIT_DETAIL_FRAME.fullmatch(lines[detail[frame]])
            require(match is not None and int(match.group("frame")) == frame,
                    "a UNIT_DETAIL_FRAME line does not read or has the wrong frame")
            row = match.groupdict()
            average, dot, ratio = (float(row[key]) for key in ("average", "dot", "ratio"))
            units = int(row["units"])
            require(all(math.isfinite(value) and value >= 0 for value in (average, dot, ratio)),
                    "detail sizes are not finite nonnegative values")
            require(dot == UNIT_DOT_PIXELS and ratio == UNIT_MODEL_TO_DOT_RATIO,
                    "detail thresholds differ from the model transition")
            require(units <= UINT32_MASK and (units != 0 or average == 0), "invalid detail reference count")
            require(row["automatic"] == "1" or (units == 0 and average == 0),
                    "manual detail has automatic reference measurements")
    return [line for line, tag in zip(lines, tags) if tag not in UNIT_DETAIL_TAGS]


def require_frame_work(result, condition, message):
    """Reject invalid optional work records before any transcript projection."""
    if not condition:
        fail(result, "frame work records: " + message)


def frame_work_numbers(result, match, names, maximum):
    """Read canonical bounded unsigned counters without accepting oversized integer strings."""
    values = {name: match.group(name) for name in names}
    require_frame_work(result, all(len(value) <= len(str(maximum)) for value in values.values()),
                       "work count exceeds its unsigned source width")
    numbers = {name: int(value) for name, value in values.items()}
    require_frame_work(result, all(value <= maximum for value in numbers.values()),
                       "work count exceeds its unsigned source width")
    return numbers


def check_shadow_index_counts(result, match):
    """Validate drawn shadow corners, uploaded words and the bounded retained pattern cache."""
    legacy = match.re is LEGACY_SHADOW_INDEX_FRAME
    fields = LEGACY_SHADOW_INDEX_UINT64_FIELDS if legacy else SHADOW_INDEX_UINT64_FIELDS
    row = frame_work_numbers(result, match, fields, UINT64_MASK)
    row.update(frame_work_numbers(result, match, SHADOW_INDEX_UINT32_FIELDS, UINT32_MASK))

    def require(condition, message):
        require_frame_work(result, condition, message)

    require(row["logical_indices"] == row["dynamic_indices"] + row["cached_indices"],
            "logical shadow indices differ from dynamic and cached draws")
    require(all(row[name] % 3 == 0
                for name in ("logical_indices", "dynamic_indices", "cached_indices")),
            "drawn shadow indices are not triangle corners")
    require(row["requested_runs"] == row["cache_hits"] + row["cache_misses"] ==
            row["cached_runs"] + row["fallback_runs"],
            "shadow requests, cache lookups and retained runs disagree")
    if legacy:
        require(row["cache_hits"] <= row["cached_runs"], "shadow cache hits exceed cached runs")
        require(row["admitted_patterns"] <= row["cache_misses"],
                "legacy shadow admissions exceed cache misses")
    require(row["admitted_patterns"] <= min(row["cached_patterns"], SHADOW_FRAME_ADMISSIONS),
            "shadow admissions exceed resident patterns or the frame limit")
    require(row["cached_patterns"] <= SHADOW_CACHE_PATTERNS,
            "shadow pattern count exceeds its cache limit")
    require(row["cache_bytes"] % SHADOW_INDEX_BYTES == 0 and
            row["cache_bytes"] <= SHADOW_CACHE_BYTES,
            "shadow cache bytes are unaligned or exceed the cache limit")
    require(row["template_uploaded_indices"] % 3 == 0 and
            row["template_uploaded_indices"] * SHADOW_INDEX_BYTES <= SHADOW_UPLOAD_BYTES,
            "shadow template upload exceeds the frame limit or is not triangle corners")
    require(row["template_uploaded_indices"] * SHADOW_INDEX_BYTES <= row["cache_bytes"],
            "shadow template upload exceeds resident cache bytes")
    require((row["cached_indices"] > 0) == (row["cached_runs"] > 0),
            "cached shadow draws disagree with cached run coverage")
    require(row["cached_runs"] == 0 or
            (row["cached_patterns"] > 0 and row["cache_bytes"] > 0),
            "cached shadow runs have no resident template")
    require((row["cached_patterns"] == 0) == (row["cache_bytes"] == 0),
            "shadow resident patterns disagree with occupied cache bytes")
    require((row["admitted_patterns"] > 0) == (row["template_uploaded_indices"] > 0),
            "shadow admissions disagree with template uploads")
    require(row["cache_hits"] == 0 or row["cached_patterns"] > 0,
            "shadow cache hits have no resident pattern")


def check_plan_work_counts(result, match):
    """Validate sampled planning clocks independently of overlapping frame and worker time."""
    fields = (PLAN_WORK_FIELDS if match.group(ROOT_PREFIX_WORK_FIELDS[0]) is not None
              else LEGACY_PLAN_WORK_FIELDS)
    names = ("source_frame", *fields,
             *(f"{phase}_{name}" for phase in PLAN_WORK_PHASES for name in PLAN_PHASE_FIELDS))
    row = frame_work_numbers(result, match, names, UINT64_MASK)
    for phase in PLAN_WORK_PHASES:
        calls, samples, wall, cpu, cpu_samples = (row[f"{phase}_{name}"]
                                                for name in PLAN_PHASE_FIELDS)
        require_frame_work(result, cpu_samples <= samples <= calls,
                           "planning phase samples exceed their calls")
        require_frame_work(result, samples != 0 or wall == 0,
                           "unsampled planning phase has wall time")
        require_frame_work(result, cpu_samples != 0 or cpu == 0,
                           "planning phase without CPU samples has CPU time")


def check_render_work_records(result, lines):
    """Validate complete optional work families in raw companion order, then remove only them."""
    tags = [line.split()[1] for line in lines]
    for prefix, known in (("SHADOW_INDEX", "SHADOW_INDEX_FRAME"), ("PLAN_WORK", "PLAN_WORK_FRAME")):
        require_frame_work(result, not any(tag.startswith(prefix) and tag != known for tag in tags),
                           "unknown " + prefix + " metadata tag")
    families = (("SHADOW_INDEX_FRAME", SHADOW_INDEX_FRAME, check_shadow_index_counts),
                ("PLAN_WORK_FRAME", PLAN_WORK_FRAME, check_plan_work_counts))
    rows = {tag: [index for index, found in enumerate(tags) if found == tag]
            for tag, _, _ in families}
    if not any(rows.values()):
        return lines

    def require(condition, message):
        require_frame_work(result, condition, message)

    positions = [index for index, tag in enumerate(tags) if tag == "FRAME"]
    results = [FRAME_RESULT.fullmatch(line) for line in lines if line.startswith("bench: RESULT ")]
    require(len(results) == 1 and results[0] is not None, "missing or malformed RESULT")
    frames = int(results[0].group(2))
    require(frames > 0 and len(positions) == frames, "work metadata needs the complete FRAME span")
    require(all(not found or len(found) == frames for found in rows.values()),
            "each FRAME needs exactly one row of each present work family")
    previous = (("UNIT_DETAIL_FRAME", UNIT_DETAIL_FRAME), ("BODY_LOD_FRAME", BODY_LOD_FRAME),
                ("SCENE_WORK_FRAME", SCENE_WORK_FRAME))
    previous_tags = frozenset(tag for tag, _ in previous)
    shadow_format = None
    for frame, position in enumerate(positions):
        require(position + 1 < len(lines), "missing SIM_FRAME after FRAME")
        regular = FRAME.fullmatch(lines[position])
        simulation = SIM_FRAME.fullmatch(lines[position + 1])
        require(regular is not None and int(regular.group(1)) == frame,
                "FRAME indices are not complete and ordered")
        require(simulation is not None and int(simulation.group("frame")) == frame,
                "work rows need adjacent FRAME and SIM_FRAME records")
        cursor = position + 2
        for tag, pattern in previous:
            if cursor < len(lines) and tags[cursor] == tag:
                companion = pattern.fullmatch(lines[cursor])
                require(companion is not None and int(companion.group("frame")) == frame,
                        "an earlier companion does not read or has the wrong frame")
                cursor += 1
        for tag, pattern, validate in families:
            if not rows[tag]:
                continue
            require(rows[tag][frame] == cursor, "work rows must follow their ordered frame companions")
            match = pattern.fullmatch(lines[cursor])
            if tag == "SHADOW_INDEX_FRAME":
                if match is None:
                    match = LEGACY_SHADOW_INDEX_FRAME.fullmatch(lines[cursor])
                if match is not None:
                    current_format = match.re is SHADOW_INDEX_FRAME
                    require(shadow_format is None or shadow_format == current_format,
                            "shadow index formats differ within one frame log")
                    shadow_format = current_format
            require(match is not None, "a " + tag + " line does not read")
            identity = frame_work_numbers(result, match, ("frame",), UINT32_MASK)
            require(identity["frame"] == frame, "work row index differs from FRAME")
            validate(result, match)
            cursor += 1
        end = positions[frame + 1] if frame + 1 < frames else len(lines)
        require(not any(tags[index] in previous_tags for index in range(cursor, end)),
                "detail, body and scene companions must precede work diagnostics")
    return [line for line, tag in zip(lines, tags) if tag not in FRAME_WORK_TAGS]


def check_work_record_presence(result, lines, plan_enabled=False):
    """Require current native producers while general parsers continue to admit older logs."""
    frames = sum(line.startswith("bench: FRAME ") for line in lines)
    shadows = [SHADOW_INDEX_FRAME.fullmatch(line) for line in lines
               if line.startswith("bench: SHADOW_INDEX_FRAME")]
    plans = [PLAN_WORK_FRAME.fullmatch(line) for line in lines
             if line.startswith("bench: PLAN_WORK_FRAME")]
    if frames == 0 or len(shadows) != frames or not all(shadows):
        fail(result, "the native frame log omitted its shadow index coverage")
    if not plan_enabled:
        if plans:
            fail(result, "the native frame log enabled planning work without an opt-in")
        return
    if len(plans) != frames or not all(plans):
        fail(result, "the opt-in native frame log omitted its planning work coverage")
    if not all(row.group(ROOT_PREFIX_WORK_FIELDS[0]) is not None for row in plans):
        fail(result, "the opt-in native frame log omitted its root-prefix activation coverage")
    if not any(row.group("completed") == "1" and
               any(int(row.group(phase + "_calls")) != 0 for phase in PLAN_WORK_PHASES)
               for row in plans):
        fail(result, "the opt-in native frame log has no completed planning attempt")


def check_root_prefix_activation(result, lines):
    """Require actual completed recipe publication, Scene ownership and successful GPU draws."""
    if result.returncode != 0:
        fail(result, f"the root-prefix activation run exited with {result.returncode}")
    check_render_work_records(result, lines)
    check_work_record_presence(result, lines, plan_enabled=True)
    totals = dict.fromkeys(ROOT_PREFIX_WORK_FIELDS, 0)
    for line in lines:
        if not line.startswith("bench: PLAN_WORK_FRAME"):
            continue
        row = PLAN_WORK_FRAME.fullmatch(line)
        if row.group("completed") != "1" or int(row.group("source_frame")) == 0:
            continue
        for name in totals:
            totals[name] += int(row.group(name))
    missing = [name for name, total in totals.items() if total == 0]
    if missing:
        fail(result, "root-prefix activation has no completed positive counts: " + ", ".join(missing))


def check_scene_work_records(result, lines):
    """Validate retained worker ranges independently of optional detail and body companions."""
    tags = [line.split()[1] for line in lines]
    if any(tag.startswith("SCENE_WORK") and tag != "SCENE_WORK_FRAME" for tag in tags):
        fail(result, "scene work records: unknown scene work metadata tag")
    work_positions = [index for index, tag in enumerate(tags) if tag == "SCENE_WORK_FRAME"]
    if not work_positions:
        return lines

    def require(condition, message):
        if not condition:
            fail(result, "scene work records: " + message)

    positions = [index for index, tag in enumerate(tags) if tag == "FRAME"]
    results = [FRAME_RESULT.fullmatch(line) for line in lines if line.startswith("bench: RESULT ")]
    require(len(results) == 1 and results[0] is not None, "missing or malformed RESULT")
    frames = int(results[0].group(2))
    require(frames > 0 and len(positions) == len(work_positions) == frames,
            "each FRAME needs exactly one SCENE_WORK_FRAME")
    for frame, position in enumerate(positions):
        require(position + 2 < len(lines), "missing work row after SIM_FRAME")
        regular = FRAME.fullmatch(lines[position])
        simulation = SIM_FRAME.fullmatch(lines[position + 1])
        require(regular is not None and int(regular.group(1)) == frame,
                "FRAME indices are not complete and ordered")
        require(simulation is not None and int(simulation.group("frame")) == frame,
                "work rows need adjacent FRAME and SIM_FRAME records")
        cursor = position + 2
        for tag, pattern in (("UNIT_DETAIL_FRAME", UNIT_DETAIL_FRAME),
                             ("BODY_LOD_FRAME", BODY_LOD_FRAME)):
            if cursor < len(lines) and tags[cursor] == tag:
                companion = pattern.fullmatch(lines[cursor])
                require(companion is not None and int(companion.group("frame")) == frame,
                        "a detail or body companion does not read or has the wrong frame")
                cursor += 1
        require(work_positions[frame] == cursor,
                "SCENE_WORK_FRAME must immediately follow its frame companions")
        require(cursor + 1 == len(lines) or
                tags[cursor + 1] not in ("UNIT_DETAIL_FRAME", "BODY_LOD_FRAME"),
                "detail and body companions must precede SCENE_WORK_FRAME")
        match = SCENE_WORK_FRAME.fullmatch(lines[cursor])
        require(match is not None and int(match.group("frame")) == frame,
                "a SCENE_WORK_FRAME line does not read or has the wrong frame")
        row = {key: int(value) for key, value in match.groupdict().items() if key != "frame"}
        require(all(value <= UINT32_MASK for value in row.values()), "work count exceeds uint32")
        bands, slots = row["candidate_bands"], row["candidate_slots"]
        accepted = row["accepted_bands"], row["accepted_slots"]
        fallback = row["fallback_frames"]
        require(bands == 0 or 2 <= bands <= SCENE_WORK_MAX_BANDS,
                "candidate band count is outside the parallel range")
        require((bands == 0 and slots == 0) or (bands != 0 and slots >= bands),
                "candidate slots disagree with their bands")
        require(accepted == (0, 0) or accepted == (bands, slots),
                "accepted work must retain the complete candidate range")
        require(fallback in (0, 1) and fallback == int(bands != 0 and accepted == (0, 0)),
                "fallback must identify a rejected nonempty candidate range")
    return [line for line, tag in zip(lines, tags) if tag != "SCENE_WORK_FRAME"]


def check_body_lod_records(result, lines):
    """Validate actual emitted body counts and remove only their complete frame family."""
    tags = [line.split()[1] for line in lines]
    if any(tag.startswith("BODY_") and tag != "BODY_LOD_FRAME" for tag in tags):
        fail(result, "body LOD records: unknown body metadata tag")
    body_positions = [index for index, tag in enumerate(tags) if tag == "BODY_LOD_FRAME"]
    if not body_positions:
        return lines

    def require(condition, message):
        if not condition:
            fail(result, "body LOD records: " + message)

    positions = [index for index, tag in enumerate(tags) if tag == "FRAME"]
    results = [FRAME_RESULT.fullmatch(line) for line in lines if line.startswith("bench: RESULT ")]
    require(len(results) == 1 and results[0] is not None, "missing or malformed RESULT")
    frames = int(results[0].group(2))
    require(frames > 0 and len(positions) == len(body_positions) == frames,
            "each FRAME needs exactly one BODY_LOD_FRAME")
    for frame, position in enumerate(positions):
        require(position + 3 < len(lines), "missing body row after unit detail")
        regular = FRAME.fullmatch(lines[position])
        simulation = SIM_FRAME.fullmatch(lines[position + 1])
        detail = UNIT_DETAIL_FRAME.fullmatch(lines[position + 2])
        require(regular is not None and int(regular.group(1)) == frame,
                "FRAME indices are not complete and ordered")
        require(simulation is not None and int(simulation.group("frame")) == frame and
                detail is not None and int(detail.group("frame")) == frame,
                "body rows need adjacent FRAME, SIM_FRAME and UNIT_DETAIL_FRAME records")
        require(body_positions[frame] == position + 3,
                "BODY_LOD_FRAME must immediately follow UNIT_DETAIL_FRAME")
        match = BODY_LOD_FRAME.fullmatch(lines[body_positions[frame]])
        require(match is not None and int(match.group("frame")) == frame,
                "a BODY_LOD_FRAME line does not read or has the wrong frame")
        row = {key: int(value) for key, value in match.groupdict().items()}
        require(all(row[key] <= UINT32_MASK for key in ("full", "body", "distance", "budget")),
                "model count exceeds uint32")
        require(all(row[key] <= UINT64_MASK
                    for key in ("full_vertices", "body_vertices", "shadow", "deferred")),
                "geometry count exceeds uint64")
        for models, vertices in (("full", "full_vertices"), ("body", "body_vertices")):
            require(row[vertices] % 3 == 0 and
                    ((row[models] == 0 and row[vertices] == 0) or
                     (row[models] > 0 and row[vertices] >= row[models] * 3)),
                    "emitted model and triangle counts disagree")
        require(row["shadow"] % 3 == 0, "shadow count is not triangle corners")
    return [line for line, tag in zip(lines, tags) if tag != "BODY_LOD_FRAME"]


def check_simulation_records(result, lines, frames=None):
    """Validate paired simulation rows before projecting the unchanged FRAME transcript."""
    lines = check_render_work_records(result, lines)
    lines = check_scene_work_records(result, lines)
    lines = check_body_lod_records(result, lines)
    lines = check_unit_detail_records(result, lines)
    positions = [index for index, line in enumerate(lines) if line.startswith("bench: FRAME")]
    simulation = [line for line in lines if line.startswith("bench: SIM_FRAME")]
    if not positions:
        if simulation:
            fail(result, "simulation records: SIM_FRAME has no FRAME span")
        return lines

    def require(condition, message):
        if not condition:
            fail(result, "simulation records: " + message)

    result_line = next((line for line in lines if line.startswith("bench: RESULT ")), "")
    span = re.search(r"\bframes (\d+)\b.*\bticks (\d+)\b", result_line)
    require(span is not None, "missing RESULT frame and tick counts")
    result_frames, result_ticks = map(int, span.groups())
    require(frames is None or frames == result_frames, "RESULT frame count differs")
    require(len(positions) == len(simulation) == result_frames,
            "each FRAME needs exactly one SIM_FRAME")
    measuring = next((line for line in lines if line.startswith("bench: MEASURING ")), "")
    initial = re.search(r"\btick=(\d+)\b", measuring)
    require(initial is not None, "missing initial measured tick")
    previous = int(initial.group(1))
    require(previous <= UINT32_MASK and result_ticks <= UINT32_MASK, "span tick exceeds uint32")
    ticks_total = 0
    simulation_total = 0.0
    for frame, position in enumerate(positions):
        frame_match = FRAME.fullmatch(lines[position])
        require(frame_match is not None and int(frame_match.group(1)) == frame,
                "FRAME indices are not complete and ordered")
        require(position + 1 < len(lines), "missing SIM_FRAME after FRAME")
        match = SIM_FRAME.fullmatch(lines[position + 1])
        require(match is not None, "SIM_FRAME must immediately follow FRAME")
        row = match.groupdict()
        require(int(row["frame"]) == frame, "SIM_FRAME index differs from FRAME")
        before, after, ticks, steps, slowest = (int(row[name]) for name in
                                                 ("before", "after", "ticks", "steps", "slowest"))
        require(all(value <= UINT32_MASK for value in (before, after, ticks, steps, slowest)),
                "SIM_FRAME count or tick exceeds uint32")
        require(before == previous, "frame tick spans are not contiguous")
        require(ticks == ((after - before) & UINT32_MASK), "ticks differ from wrapped boundaries")
        milliseconds, maximum = float(row["sim"]), float(row["maximum"])
        require(milliseconds < float("inf") and maximum < float("inf"), "nonfinite step time")
        if steps == 0:
            require(milliseconds == maximum == slowest == 0, "zero-step frame has timed work")
        else:
            require(maximum <= milliseconds + SIM_ROUNDING_MS, "step maximum exceeds frame sum")
            require(milliseconds <= steps * maximum + (steps + 1) * SIM_ROUNDING_MS,
                    "frame sum exceeds its step maxima")
            if ticks != 0:
                require(0 < ((slowest - before) & UINT32_MASK) <= ticks,
                        "slowest step tick is outside the frame")
        ticks_total += ticks
        simulation_total += milliseconds
        previous = after
    require((ticks_total & UINT32_MASK) == result_ticks, "frame ticks differ from RESULT")
    measured = next((match for line in lines if (match := MEASURE.fullmatch(line)) and
                     match.group(1) == "sim(per frame)"), None)
    require(measured is not None and
            abs(simulation_total / result_frames - float(measured.group(3))) <= SIM_MEAN_ROUNDING_MS,
            "frame simulation mean differs from MEASURE")
    return [line for line in lines if not line.startswith("bench: SIM_FRAME")]


def check_renderer_records(result, lines, frames, timers):
    """Validate optional renderer families, returning the unchanged legacy transcript."""
    lines = check_render_work_records(result, lines)
    lines = check_scene_work_records(result, lines)
    lines = check_body_lod_records(result, lines)
    lines = check_unit_detail_records(result, lines)
    legacy = [line for line in lines if line.split()[1] not in RENDERER_TAGS]
    lines = check_simulation_records(result, lines, frames)
    tags = [line.split()[1] for line in lines]
    if not any(tag in RENDERER_TAGS for tag in tags):
        return legacy

    def require(condition, message):
        if not condition:
            fail(result, "renderer records: " + message)

    require(frames > 0, "counter span must contain frames")
    positions = [index for index, tag in enumerate(tags) if tag == "FRAME"]
    require(len(positions) == frames, "counter frames need the complete FRAME span")
    require(all(FRAME.fullmatch(lines[index]) for index in positions), "a FRAME line does not read")
    require([int(FRAME.fullmatch(lines[index]).group(1)) for index in positions] == list(range(frames)),
            "FRAME indices are not in order")
    results = [index for index, tag in enumerate(tags) if tag == "RESULT"]
    require(len(results) == 1 and results[0] < positions[0], "counters must follow RESULT")
    family_tags = (("GROUND_INDEX", "GROUND_INDEX_FRAME"), ("POSE", "POSE_FRAME"),
                   ("MATERIAL", "MATERIAL_FRAME"))
    present = {summary: summary in tags or per_frame in tags
               for summary, per_frame in family_tags}
    builds_present = any(tag.startswith("GROUND_INDEX_BUILD_") for tag in tags)
    require(not builds_present or present["GROUND_INDEX"], "BUILD needs ground-index counters")
    summary_positions = [index for index, tag in enumerate(tags)
                         if tag in ("GROUND_INDEX", "POSE", "MATERIAL")]
    lower = max([results[0], *[index for index, tag in enumerate(tags) if tag == "SLOW"]])
    require(all(lower < index < positions[0] for index in summary_positions),
            "summaries must follow SLOW and precede FRAME")
    expected_summaries = (["GROUND_INDEX"] if present["GROUND_INDEX"] else []) + \
        (["POSE"] if present["POSE"] else []) + \
        (["MATERIAL"] * len(MATERIAL_KINDS) if present["MATERIAL"] else [])
    require([tags[index] for index in summary_positions] == (expected_summaries if timers else []),
            "summary families are missing, repeated or out of order")
    sums = {"GROUND_INDEX": [0, 0, 0, 0.0], "POSE": [0, 0, 0]}
    material_sums = {kind: [0, 0, 0] for kind in MATERIAL_KINDS}
    consumed = set(summary_positions)
    done = [index for index, tag in enumerate(tags) if tag == "DONE"]
    require(len(done) == 1 and done[0] > positions[-1], "counter span needs final DONE")

    def read(pattern, index):
        match = pattern.fullmatch(lines[index])
        require(match is not None, "a line does not read: " + lines[index])
        consumed.add(index)
        return match

    for frame, first in enumerate(positions):
        end = positions[frame + 1] if frame + 1 < frames else done[0]
        cursor = first + 1
        ground = None
        if present["GROUND_INDEX"]:
            require(cursor < end, "missing GROUND_INDEX_FRAME")
            match = read(GROUND_INDEX_FRAME, cursor)
            require(int(match.group(1)) == frame, "wrong ground-index frame")
            ground = [int(match.group(n)) for n in (2, 3, 4)] + [float(match.group(5))]
            require(ground[0] != 0 or ground[2:] == [0, 0.0],
                    "ground-index frame without builds has construction work")
            sums["GROUND_INDEX"] = [a + b for a, b in zip(sums["GROUND_INDEX"], ground)]
            cursor += 1
        records = []
        while cursor < end and tags[cursor] == "GROUND_INDEX_BUILD_FRAME":
            match = read(INDEX_BUILD, cursor)
            row = match.groupdict()
            require(int(row["frame"]) == frame and int(row["ordinal"]) == len(records),
                    "BUILD frame or ordinal is out of order")
            require(len(records) < INDEX_BUILD_CAPACITY, "too many BUILD records")
            integer_fields = ("previous_attempts", "old_revision", "revision", "old_vertices", "vertices")
            require(all(int(row[name]) < 1 << 64 for name in integer_fields), "BUILD word exceeds uint64")
            require(int(row["old_vertices"]) % 3 == 0 and int(row["vertices"]) % 3 == 0,
                    "BUILD vertices are not complete triangles")
            expected = (0 if int(row["was_valid"]) else 1) | (2 if int(row["revision"]) == 0 else 0)
            for bit, old, new in ((4, "old_revision", "revision"), (8, "old_vertices", "vertices"),
                                  (16, "old_card", "card"), (32, "old_axes", "axes"),
                                  (64, "old_fp", "fp")):
                if old.endswith(("axes", "fp")):
                    before = tuple(int(word, 16) for word in row[old].split(","))
                    requested = tuple(int(word, 16) for word in row[new].split(","))
                else:
                    before, requested = int(row[old]), int(row[new])
                expected |= bit if before != requested else 0
            require(int(row["reasons"]) == expected and expected != 0, "BUILD reason mask disagrees with keys")
            require(not int(row["was_valid"]) or int(row["previous_attempts"]) != 0,
                    "valid cache has no previous attempt")
            bounds, world, card_ms, wall = (float(row[name]) for name in
                                            ("bounds", "world", "card_ms", "wall"))
            require(all(0 <= value < float("inf") for value in (bounds, world, card_ms, wall)),
                    "BUILD spans are not finite nonnegative times")
            if row["parallel"] == "1":
                require(row["card"] == "1", "parallel BUILD has no card constructor")
                require(bounds + max(world, card_ms) <= wall + INDEX_BUILD_ROUNDING_MS,
                        "parallel BUILD spans exceed wall time")
            else:
                require(abs(bounds + world + card_ms - wall) <= INDEX_BUILD_ROUNDING_MS,
                        "BUILD subspans do not partition wall time")
            records.append(row)
            cursor += 1
        dropped = 0
        if cursor < end and tags[cursor] == "GROUND_INDEX_BUILD_DROPPED_FRAME":
            match = read(INDEX_DROPPED, cursor)
            dropped = int(match.group(2))
            require(int(match.group(1)) == frame and dropped > 0 and
                    len(records) == INDEX_BUILD_CAPACITY, "invalid BUILD overflow")
            cursor += 1
        if records or dropped or (builds_present and ground is not None and ground[0] != 0):
            require(timers and ground is not None and len(records) + dropped == ground[0],
                    "BUILD count differs from ground-index frame")
            require(sum(int(row["vertices"]) // 3 for row in records) <= ground[2],
                    "BUILD triangles exceed frame count")
            if dropped == 0:
                require(sum(int(row["vertices"]) // 3 for row in records) == ground[2],
                        "BUILD triangle count differs from frame")
                require(abs(sum(float(row["wall"]) for row in records) - ground[3]) <=
                        0.001 * (len(records) + 1), "BUILD wall differs from ground-index frame")
        if present["POSE"]:
            require(cursor < end, "missing POSE_FRAME")
            match = read(POSE_FRAME, cursor)
            values = [int(match.group(n)) for n in (2, 3, 4)]
            # A model-view return can prime the current rig after this tick's
            # root was captured by Dots, so piece work need not add a root.
            require(int(match.group(1)) == frame and values[2] <= values[1],
                    "invalid pose frame")
            sums["POSE"] = [a + b for a, b in zip(sums["POSE"], values)]
            cursor += 1
        if present["MATERIAL"]:
            for kind in MATERIAL_KINDS:
                require(cursor < end, "missing MATERIAL_FRAME")
                match = read(MATERIAL_FRAME, cursor)
                require(int(match.group(1)) == frame and match.group(2) == kind,
                        "material classes or frames are out of order")
                values = [int(match.group(n)) for n in (3, 4, 5)]
                require(values[1] >= values[0] and
                        (values[0] != 0 or values[1:] == [0, 0]), "invalid material texel count")
                material_sums[kind] = [a + b for a, b in zip(material_sums[kind], values)]
                cursor += 1
        require(cursor == end, "unexpected or misplaced record in FRAME block")
    for index, tag in enumerate(tags):
        require(tag not in RENDERER_TAGS or index in consumed, "renderer record is outside its span")
    if timers:
        for family, pattern in (("GROUND_INDEX", GROUND_INDEX), ("POSE", POSE)):
            if not present[family]:
                continue
            index = next(index for index in summary_positions if tags[index] == family)
            values = read(pattern, index).groups()
            actual = ([int(value) for value in values[:3]] + [float(values[3])]
                      if family == "GROUND_INDEX" else [int(value) for value in values])
            expected = sums[family]
            require(all(abs(a - b) <= (0.00051 * (frames + 1) if family == "GROUND_INDEX" and
                                      n == 3 else 0) for n, (a, b) in enumerate(zip(actual, expected))),
                    family + " summary differs from frame totals")
        for index in [index for index in summary_positions if tags[index] == "MATERIAL"]:
            match = read(MATERIAL, index)
            require(match.group(1) in material_sums and
                    [int(match.group(n)) for n in (2, 3, 4)] == material_sums[match.group(1)],
                    "material summary differs from frame totals")
        require([read(MATERIAL, index).group(1) for index in summary_positions
                 if tags[index] == "MATERIAL"] == (list(MATERIAL_KINDS) if present["MATERIAL"] else []),
                "material summary classes are out of order")
    else:
        require(not builds_present and not any(sums[family] != [0] * len(sums[family])
                                              for family in sums) and
                not any(any(values) for values in material_sums.values()),
                "timers-off records contain measured work")
    return legacy


def check_lines(result, timers, rules_version=0, requested_rules_version=0):
    """Checks a finished run's lines and returns them."""
    if result.returncode != 0:
        fail(result, f"the bench exited with {result.returncode}")
    check_initial(result, rules_version, requested_rules_version)
    lines = [line for line in result.stdout.splitlines() if line.startswith("bench: ")]
    # A window the dummy driver leaves away is noted after RESULT.
    away = [line for line in lines if line.startswith("bench: AWAY")]
    if away and not AWAY.match(away[0]):
        fail(result, f"the AWAY line does not read: {away[0]}")
    lines = [line for line in lines if not line.startswith("bench: AWAY")]
    original = lines
    lines = check_render_work_records(result, lines)
    lines = check_scene_work_records(result, lines)
    lines = check_body_lod_records(result, lines)
    lines = check_unit_detail_records(result, lines)
    simulation_checked = check_simulation_records(result, lines)
    kinds = [line.split()[1] for line in simulation_checked if line.split()[1] not in RENDERER_TAGS]
    # The lines come in their order, each kind once but SEC, MEASURE, STAGE,
    # ATLAS, SLOW and FRAME.
    order = ["INITIAL", "WARMUP", "MEASURING", "SEC", "SHOT", "RESULT", "MEASURE", "STAGE", "COUNTS",
             "GROUND", "ATLAS", "SLOW", "FRAME", "DONE"]
    if not timers:
        for kind in ("STAGE", "SLOW", "FRAME"):
            order.remove(kind)
    runs = [kind for index, kind in enumerate(kinds) if index == 0 or kinds[index - 1] != kind]
    # SHOT falls within the seconds.
    runs = [kind for kind in runs if kind != "SEC"]
    expected = [kind for kind in order if kind != "SEC"]
    if runs != expected:
        fail(result, f"the bench's lines came as {runs}, not {expected}")
    if kinds.count("SEC") < MEASURE_SECONDS - 1:
        fail(result, f"the bench printed {kinds.count('SEC')} SEC lines in {MEASURE_SECONDS} s")
    for line in lines:
        if line.startswith("bench: SEC") and not SEC.match(line):
            fail(result, f"a SEC line does not read: {line}")
    result_match = next((RESULT.match(line) for line in lines if RESULT.match(line)), None)
    if not result_match:
        fail(result, "the RESULT line does not read")
    seconds, frames, fps, low1_fps = (float(result_match.group(index)) for index in (1, 2, 3, 4))
    if frames < 1 or not 0 < seconds <= MEASURE_SECONDS + 1:
        fail(result, f"the bench measured {frames:.0f} frames in {seconds} s")
    # The seconds are printed to two places, so the rate agrees within 1%.
    if abs(fps - frames / seconds) > 0.01 * fps or not 0 < low1_fps <= fps + 0.01:
        fail(result, f"the bench's frame rates {fps} and {low1_fps} do not agree")
    measured = {}
    for line in lines:
        if line.startswith("bench: MEASURE") and not TICK.match(line):
            match = MEASURE.match(line)
            if not match:
                fail(result, f"a MEASURE line does not read: {line}")
            least, mean, p50, p95, p99, low1, most = (float(value) for value in match.groups()[1:])
            if not ordered([least, p50, p95, p99, most]) or not least <= mean <= most or \
                    not p50 <= low1 <= most:
                fail(result, f"a spread is out of order: {line}")
            measured[match.group(1)] = (mean, low1)
    if tuple(measured) != MEASURES or not any(TICK.match(line) for line in lines):
        fail(result, f"the bench measured {tuple(measured)}, not {MEASURES} and the tick")
    if abs(measured["frame"][1] - 1000 / low1_fps) > 0.05 * measured["frame"][1]:
        fail(result, "the 1% low frame rate is not the slowest 1% of the frames")
    stages = [STAGE.match(line) for line in lines if line.startswith("bench: STAGE")]
    names = tuple(match.group(1).rstrip() if match else "" for match in stages)
    if timers and names != STAGES:
        fail(result, f"the stage timers are {names}, not {STAGES}")
    counts = next((COUNTS.match(line) for line in lines if COUNTS.match(line)), None)
    ground = next((GROUND.match(line) for line in lines if GROUND.match(line)), None)
    atlas = next((ATLAS.match(line) for line in lines if ATLAS.match(line)), None)
    if not counts or not ground or not atlas:
        fail(result, "the COUNTS, GROUND or ATLAS line does not read")
    if int(counts.group(1)) < LEAST_UNITS or int(ground.group(2)) != frames or \
            int(atlas.group(2)) != frames:
        fail(result, f"the bench counted {counts.group(1)} units and other frames than {frames:.0f}")
    check_renderer_records(result, lines, int(frames), timers)
    if timers:
        check_slow_and_frame_lines(result, lines, int(frames), measured["frame"])
    return original


def check_slow_and_frame_lines(result, lines, frames, frame_spread):
    """Checks the SLOW lines of the slowest 1% of frames and the FRAME lines of every frame."""
    slow = next((SLOW.match(line) for line in lines if SLOW.match(line)), None)
    if not slow:
        fail(result, "the SLOW line does not read")
    slowest, span, slow_mean = int(slow.group(1)), int(slow.group(2)), float(slow.group(3))
    if span != frames or slowest != (frames + 99) // 100:
        fail(result, f"the slowest 1% are {slowest} of {span} frames, not of {frames}")
    # The slowest 1% are the frames the 1% low is the mean of.
    if abs(slow_mean - frame_spread[1]) > 0.01 + 0.001 * frame_spread[1]:
        fail(result, f"the slowest frames' mean {slow_mean} is not the 1% low {frame_spread[1]}")
    slow_stages = [SLOW_STAGE.match(line) for line in lines if line.startswith("bench: SLOW STAGE")]
    names = tuple(match.group(1).rstrip() if match else "" for match in slow_stages)
    if names != STAGES:
        fail(result, f"the slowest frames' stage timers are {names}, not {STAGES}")
    for line in lines:
        if line.startswith("bench: SLOW ATLAS") and not SLOW_ATLAS.match(line):
            fail(result, f"a SLOW ATLAS line does not read: {line}")
    logged = [FRAME.match(line) for line in lines if line.startswith("bench: FRAME")]
    if len(logged) != frames or not all(logged) or \
            [int(match.group(1)) for match in logged] != list(range(frames)):
        fail(result, f"the frame log does not hold the span's {frames} frames in order")
    # The frame log's times are the frames the spread is taken from; they
    # are printed to two places.
    mean = sum(float(match.group(2)) for match in logged) / frames
    if abs(mean - frame_spread[0]) > 0.02:
        fail(result, f"the frame log's mean frame {mean:.3f} is not the span's {frame_spread[0]}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--scratch-root", type=Path)
    arguments = parser.parse_args()
    # The runs start in the scratch folder.
    arguments.native = arguments.native.absolute()
    arguments.game_dir = arguments.game_dir.absolute()
    if arguments.scratch_root:
        arguments.scratch_root.mkdir(parents=True, exist_ok=True)
    stage = BENCH / "battle.stage"
    try:
        with tempfile.TemporaryDirectory(dir=arguments.scratch_root) as scratch:
            workdir = Path(scratch)
            timed = run_bench(arguments.native, arguments.game_dir, workdir, "timed", VIEW.format(
                stage=stage, warmup=WARMUP_TICKS, seconds=MEASURE_SECONDS, timers=1))
            lines = check_lines(timed, True)
            check_work_record_presence(timed, lines)
            for family in ("GROUND_INDEX", "POSE", "MATERIAL"):
                if not any(line.split()[1] == family for line in lines):
                    fail(timed, f"the timed bench omitted its {family} renderer counters")
            queued = QUEUED.findall(timed.stdout)
            if len(queued) != 12 or ("60", "ARMWAR") not in queued or ("30", "CORVENG") not in queued:
                fail(timed, f"the stage queued {queued}")
            if any(int(GROUND_INDEX_FRAME.fullmatch(line).group(2)) != 0
                   for line in lines if GROUND_INDEX_FRAME.fullmatch(line)) and not any(
                       line.startswith("bench: GROUND_INDEX_BUILD_FRAME ") for line in lines):
                fail(timed, "the timed bench omitted its ground-index BUILD diagnostics")
            # The summariser reads the run.
            log = workdir / "timed" / "bench.txt"
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")
            table = subprocess.run(
                [sys.executable, str(BENCH / "summarise.py"), str(log)], timeout=60,
                check=False, capture_output=True, text=True, errors="replace")
            if table.returncode != 0 or "| check " not in table.stdout:
                print(table.stdout + table.stderr, end="")
                raise BenchFailure("tools/bench/summarise.py did not read the run")
            untimed = run_bench(arguments.native, arguments.game_dir, workdir, "untimed",
                                VIEW.format(stage=stage, warmup=WARMUP_TICKS, seconds=1, timers=0))
            if any(line.startswith(("bench: STAGE", "bench: SLOW", "bench: FRAME", "bench: SIM_FRAME",
                                    "bench: SHADOW_INDEX", "bench: PLAN_WORK")) or
                   (line.startswith("bench: ") and
                    line.split()[1] in RENDERER_TAGS | UNIT_DETAIL_TAGS)
                   for line in untimed.stdout.splitlines()):
                fail(untimed, "the bench printed its stage timers with them off")
            if untimed.returncode != 0 or "bench: DONE label=check" not in untimed.stdout:
                fail(untimed, "the bench with its timers off did not finish")
            check_initial(untimed)
            refused = run_bench(arguments.native, arguments.game_dir, workdir, "refused",
                                "label refused\nspeed 3\n")
            view = workdir / "refused" / "refused.bench"
            expected = f"bench: FAILED {view}: line 2: the bench has no key speed"
            if refused.returncode != 1 or expected not in refused.stdout.splitlines():
                fail(refused, "a view file with a key the bench does not know was not refused")

            # A paused render span must warm actual frames without advancing
            # the world and must refuse a partially staged population.
            paused_stage = workdir / "paused.stage"
            paused_stage.write_text("key Pause\n", encoding="utf-8")
            paused_view = (
                f"label paused\nstage {paused_stage}\nmenu_seconds 0\n"
                "warmup_ticks 0\nwarmup_frames 2\nmeasure_frames 3\n"
                "measure_seconds 30\nexpected_live_units 2\nrequire_paused 1\n"
                "stage_timers 0\nframe_log 1\n"
            )
            paused = run_bench(arguments.native, arguments.game_dir, workdir,
                               "paused", paused_view)
            check_initial(paused)
            if paused.returncode != 0 or not re.search(
                    r"^bench: STATIC tick \d+ units 2 world [0-9a-f]{16} "
                    r"warmup_frames 2 verified_frames 3$", paused.stdout, re.M):
                fail(paused, "the paused rendering span was not verified")
            if len(re.findall(r"^bench: FRAME ", paused.stdout, re.M)) != 3 or not re.search(
                    r"^bench: RESULT label=paused .* frames 3 .* ticks 0 ", paused.stdout, re.M):
                fail(paused, "the paused rendering span did not measure three frames without ticks")
            if not re.search(
                    r"^bench: RENDER accelerated [01] full [01] turned 0 drawn \d+ "
                    r"cpu_image_bytes \d+ cpu_image_sha256 (?:[0-9a-f]{64}|-)$",
                    paused.stdout, re.M):
                fail(paused, "the paused rendering span did not identify its actual renderer")
            check_renderer_records(
                paused, [line for line in paused.stdout.splitlines() if line.startswith("bench: ")],
                3, False)
            wrong_population = run_bench(
                arguments.native, arguments.game_dir, workdir, "wrong-population",
                paused_view.replace("expected_live_units 2", "expected_live_units 3"))
            if wrong_population.returncode != 1 or \
                    "bench: FAILED the live unit roster differs from expected_live_units" \
                    not in wrong_population.stdout:
                fail(wrong_population, "a wrong rendering population was not refused")
            for version in (0, 4, 6, 7, 8, 10, 11, 13):
                rules_view = paused_view + (f"rules_version {version}\n" if version else "")
                if version == 11:
                    rules_view += "plan_work 1\n"
                selected = run_bench(
                    arguments.native, arguments.game_dir, workdir, f"rules-{version}",
                    rules_view, enhanced=True)
                if selected.returncode != 0 or not re.search(
                        r"^bench: STATIC tick \d+ units 2 world [0-9a-f]{16} "
                        r"warmup_frames 2 verified_frames 3$", selected.stdout, re.M):
                    fail(selected, "the Enhanced paused span did not finish")
                check_initial(selected, version or CURRENT_RULES_VERSION, version)
                selected_lines = [line for line in selected.stdout.splitlines()
                                  if line.startswith("bench: ")]
                check_renderer_records(selected, selected_lines, 3, False)
                check_work_record_presence(selected, selected_lines, plan_enabled=version == 11)
            mode_off = run_bench(
                arguments.native, arguments.game_dir, workdir, "rules-off",
                "label rules-off\nrules_version 6\n")
            if mode_off.returncode != 1 or \
                    "bench: FAILED rules_version requires Enhanced Mode" \
                    not in mode_off.stdout.splitlines():
                fail(mode_off, "explicit rules without Enhanced Mode were not refused")
            for version in (9, 12, 14):
                name = f"rules-unsupported-{version}"
                unsupported = run_bench(
                    arguments.native, arguments.game_dir, workdir, name,
                    f"label {name}\nrules_version {version}\n", enhanced=True)
                view = workdir / name / (name + ".bench")
                expected = (f"bench: FAILED {view}: line 2: rules_version takes 0 or a supported "
                            "Enhanced Mode rules version")
                check_rules_rejection(unsupported, expected)
    except BenchFailure as failure:
        raise SystemExit(f"bench check: {failure}")
    print(f"bench check: the battle played, measured and reported {len(lines)} well-formed lines")


if __name__ == "__main__":
    main()
